// The bottom screen's panel as every platform shows it (second_display.h): the game's bottom picture (bottom_hud.c), the
// background before and after a quest, the wallpapers. Putting it on a display is second_display_android.c's and
// second_display_desktop.c's, or, in a DUAL_SCREEN=1 build (MHGU_SECOND_DISPLAY), the panel leased below.
//
// Second display: a panel the compositor leases to the game (drm-lease-v1), driven directly through VK_KHR_display while
// the game plays in its normal window. On Armada's gamescope that is the bottom panel of a dual-screen handheld. It shows
// the background until the game's first bottom picture of a quest (bottom_hud.c) is ready, then that picture, and the
// background again once the game draws no panel pieces (back in town).
//   [Dual Screen] SecondDisplay = On   or MHGU_SECOND_DISPLAY=1|0 (the variable wins). Default: off
//   MHGU_SECOND_DISPLAY_CONNECTOR=<name>   that connector (default: the first internal panel offered)
//   MHGU_SECOND_DISPLAY_ROTATION=0|90|180|270   the picture's turn, counter-clockwise (default: the connector's panel
//                                               orientation)
//   MHGU_SECOND_DISPLAY_TOUCH=auto|<path>|<name>|off   the panel's touchscreen, for its panels (bottom_hud.c; default
//                                                  auto). The device is grabbed: the compositor stops seeing it while
//                                                  the game runs
//   MHGU_SECOND_DISPLAY_HZ=1..240   how often the picture changes (default 60: the game's own rate)
//
// The game's frame must never wait for this panel, so it shares nothing with the game's Vulkan objects: one thread owns
// a VkInstance and a VkDevice of its own, with their own VkQueue, swapchain, command pool and memory. That thread never
// takes vk_lock; of the game's Vulkan objects it only reads the timeline semaphore's value and host memory the GPU has
// finished writing (bottom_hud_take), and the game's thread never waits for it. The panel's own vkAcquireNextImageKHR has
// timeout 0 and a frame that can't start at once is skipped.
// The game's instance and device are created exactly as without this file. Implicit layers stay off the thread's
// instance, so no overlay layer can add a lock shared with the game's present.
// The lease is held for the process' life; closing the process gives it back to the compositor.
#ifdef MHGU_SECOND_DISPLAY
#define _GNU_SOURCE
#endif
#include "../../config.h"
#ifdef MHGU_DUAL_SCREEN
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <SDL3/SDL.h>
#ifdef MHGU_SECOND_DISPLAY
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#include "drm_lease.h"
#endif

#include "../../rt.h"
#include "bottom_hud.h"
#include "second_display.h"

#define SD_LOG(...) LOG("[second-display] " __VA_ARGS__)

#ifdef MHGU_SECOND_DISPLAY
#define MAX_IMAGES 8

static uint32_t gpu_vendor, gpu_device;

static struct {
    VkInstance inst;
    VkPhysicalDevice phys;
    VkSurfaceKHR surface;
    VkDevice dev;
    VkQueue queue;
    VkSwapchainKHR swapchain;
    VkFormat format;
    VkExtent2D extent;
    uint32_t count;
    VkImage images[MAX_IMAGES];
    VkSemaphore acquired, rendered[MAX_IMAGES];
    VkFence done;
    VkCommandPool pool;
    VkCommandBuffer cb;
    VkBuffer staging;
    VkDeviceMemory staging_mem;
    uint32_t *pixels;
    DrmLeaseOutput out;
    bool have_lease;
} s;

// ---- the picture
// A panel pixel for the point (u, v) of the upright picture, which is width x height (the panel's mode, turned).
// rotation counts as DRM does, counter-clockwise: Right Side Up, Azahar's 270, puts the picture's top at the panel's right.
static void put(uint32_t u, uint32_t v, uint32_t color) {
    uint32_t pw = s.extent.width, ph = s.extent.height, x, y;
    switch (s.out.rotation) {
    case 90: x = v, y = ph - 1 - u; break;
    case 180: x = pw - 1 - u, y = ph - 1 - v; break;
    case 270: x = pw - 1 - v, y = u; break;
    default: x = u, y = v; break;
    }
    if (x < pw && y < ph) s.pixels[y * pw + x] = color;
}

static void upright_size(uint32_t *w, uint32_t *h) {
    bool turned = s.out.rotation == 90 || s.out.rotation == 270;
    *w = turned ? s.extent.height : s.extent.width;
    *h = turned ? s.extent.width : s.extent.height;
}

#endif

// ---- the wallpapers ([Dual Screen] Background = Wallpaper: Wallpaper names a BMP in wallpapers/, empty each in turn),
// loaded once when the setting changes, in the picture's own RGBA layout: a change every 30 s only picks another.
#define WALLPAPERS_MAX 16
#define WALLPAPER_SECONDS 30
#define WALLPAPER_FADE_MS 1000  // (each change fades from one to the next over this time)
static uint32_t *wallpapers[WALLPAPERS_MAX];
static int nwallpapers;

// One BMP of wallpapers/ into w x h, scaled to cover and cropped at its centre. NULL: no such file.
static uint32_t *load_wallpaper(const char *file, uint32_t w, uint32_t h) {
    char path[320];
    snprintf(path, sizeof path, "wallpapers/%s", file);
    SDL_Surface *bmp = SDL_LoadBMP(path);
    SDL_Surface *img = bmp ? SDL_ConvertSurface(bmp, SDL_PIXELFORMAT_RGBA32) : NULL;
    if (bmp) SDL_DestroySurface(bmp);
    if (!img) { SD_LOG("no wallpaper (%s): %s\n", path, SDL_GetError()); return NULL; }
    uint32_t *px = malloc((size_t)w * h * 4);
    if (px) {
        float k = fmaxf((float)w / (float)img->w, (float)h / (float)img->h);
        float x0 = ((float)img->w - (float)w / k) * 0.5f, y0 = ((float)img->h - (float)h / k) * 0.5f;
        for (uint32_t v = 0; v < h; v++) {
            const uint32_t *row = (const uint32_t *)((const uint8_t *)img->pixels + (size_t)img->pitch * (uint32_t)(y0 + (float)v / k));
            for (uint32_t u = 0; u < w; u++) px[v * w + u] = row[(uint32_t)(x0 + (float)u / k)] | 0xff000000u;
        }
        SD_LOG("wallpaper %s %dx%d\n", path, img->w, img->h);
    }
    SDL_DestroySurface(img);
    return px;
}

static int by_name(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

// The [Dual Screen] Wallpaper, or when it is empty each BMP of wallpapers/ (by name), at most max, into out. Their count:
// 0 when Background is not Wallpaper or no file loads.
static int load_wallpapers(uint32_t w, uint32_t h, uint32_t **out, int max) {
    if (g_config.bottom_background != BOTTOM_BG_WALLPAPER || max < 1) return 0;
    if (g_config.bottom_wallpaper[0]) return (*out = load_wallpaper(g_config.bottom_wallpaper, w, h)) != NULL;
    char *names[WALLPAPERS_MAX];
    int n = 0;
    DIR *d = opendir("wallpapers");
    if (!d) { SD_LOG("no wallpapers folder: %s\n", strerror(errno)); return 0; }
    for (struct dirent *e; (e = readdir(d)) && n < WALLPAPERS_MAX;) {
        size_t len = strlen(e->d_name);
        if (len > 4 && !strcasecmp(e->d_name + len - 4, ".bmp")) names[n++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(names, (size_t)n, sizeof names[0], by_name);
    int got = 0;
    for (int i = 0; i < n; i++) {
        if (got < max && names[i] && (out[got] = load_wallpaper(names[i], w, h))) got++;
        free(names[i]);
    }
    return got;
}

// out = a faded towards b by t / 256 (opaque RGBA8), two channels per multiply.
static void blend_wallpapers(uint32_t *out, const uint32_t *a, const uint32_t *b, size_t n, uint32_t t) {
    for (size_t i = 0; i < n; i++) {
        uint32_t x = a[i], y = b[i];
        uint32_t rb = (((x & 0xff00ffu) * (256 - t) + (y & 0xff00ffu) * t) >> 8) & 0xff00ffu;
        uint32_t g = (((x & 0xff00u) * (256 - t) + (y & 0xff00u) * t) >> 8) & 0xff00u;
        out[i] = 0xff000000u | rb | g;
    }
}

const uint32_t *second_display_wallpaper(uint32_t w, uint32_t h, bool *changed) {
    static int background = -1, shown, from = -1;  // (from: the one fading out, -1 none)
    static char name[sizeof g_config.bottom_wallpaper];
    static uint64_t shown_at;
    static uint32_t *faded;  // (the picture between the two while they fade)
    uint64_t now = SDL_GetTicks();
    *changed = false;
    if (background != g_config.bottom_background || strcmp(name, g_config.bottom_wallpaper)) {
        background = g_config.bottom_background;
        snprintf(name, sizeof name, "%s", g_config.bottom_wallpaper);
        for (int i = 0; i < nwallpapers; i++) free(wallpapers[i]);
        nwallpapers = load_wallpapers(w, h, wallpapers, WALLPAPERS_MAX);
        free(faded);
        faded = nwallpapers > 1 ? malloc((size_t)w * h * 4) : NULL;
        shown = 0, from = -1, shown_at = now, *changed = true;  // (a setting change shows the new one at once)
    } else if (nwallpapers > 1 && faded && now - shown_at >= WALLPAPER_SECONDS * 1000u) {
        from = shown, shown = (shown + 1) % nwallpapers, shown_at = now;
        SD_LOG("wallpaper %d of %d\n", shown + 1, nwallpapers);
    }
    if (from >= 0) {  // (fading: a new picture every call until it is done)
        *changed = true;
        uint64_t t = now - shown_at;
        if (t < WALLPAPER_FADE_MS) {
            blend_wallpapers(faded, wallpapers[from], wallpapers[shown], (size_t)w * h, (uint32_t)(t * 256 / WALLPAPER_FADE_MS));
            return faded;
        }
        from = -1;
    }
    return nwallpapers ? wallpapers[shown] : NULL;
}

// ---- the picture
typedef struct {
    uint32_t *frame, w, h;
    const uint32_t *wallpaper;
    bool quest;  // (a quest picture: its panels kept, the wallpaper round them; else all wallpaper)
} Copy;

// The game's bottom picture (RGBA8, w x h: bottom_hud.c) into the frame, opaque, with the wallpaper round the panels.
static void hud_copy(const uint32_t *rgba, uint32_t w, uint32_t h, void *ctx) {
    const Copy *c = ctx;
    if (w != c->w || h != c->h) return;
    for (uint32_t v = 0; v < h; v++)
        for (uint32_t u = 0; u < w; u++) {
            uint32_t p = rgba[v * w + u];
            if (c->wallpaper && (!c->quest || !bottom_hud_covers(u, v, w, h))) p = c->wallpaper[v * w + u];
            c->frame[v * w + u] = 0xff000000u | p;
        }
}

bool second_display_compose(uint32_t *frame, uint32_t w, uint32_t h, SecondDisplayState *st) {
    bool had_hud = st->hud_seq != 0, changed;
    Copy c = {frame, w, h, second_display_wallpaper(w, h, &changed), true};
    if (changed) st->empty_shown = false;
    if (bottom_hud_take(hud_copy, &c, &st->hud_seq)) {
        if (!had_hud) SD_LOG("showing the game's bottom picture\n");
        st->empty_shown = false;
        return true;
    }
    // (nothing new from the game: the panel keeps its picture, until no panel piece has been drawn for a while)
    if (st->empty_shown || (had_hud && !bottom_hud_stale())) return false;
    uint32_t *empty = malloc((size_t)w * h * 4);
    if (!empty) return false;
    bottom_hud_empty_picture(empty, w, h, false);  // (out of a quest: the background alone)
    c.quest = false;
    hud_copy(empty, w, h, &c);
    free(empty);
    st->empty_shown = true;
    return true;
}

void second_display_apply(void) { config_second_display_request(); }

#ifdef MHGU_SECOND_DISPLAY
// ---- Vulkan, all of it on this thread
static bool have_instance_exts(const char *const *want, uint32_t n) {
    uint32_t c = 0;
    vkEnumerateInstanceExtensionProperties(NULL, &c, NULL);
    VkExtensionProperties *p = calloc(c ? c : 1, sizeof *p);
    vkEnumerateInstanceExtensionProperties(NULL, &c, p);
    bool all = true;
    for (uint32_t i = 0; i < n; i++) {
        bool found = false;
        for (uint32_t k = 0; k < c; k++) found = found || !strcmp(p[k].extensionName, want[i]);
        if (!found) SD_LOG("the driver has no %s\n", want[i]);
        all = all && found;
    }
    free(p);
    return all;
}

static bool create_instance_for_panel(void) {
    static const char *const ext[] = {
        VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_DISPLAY_EXTENSION_NAME, VK_EXT_DIRECT_MODE_DISPLAY_EXTENSION_NAME,
        VK_EXT_ACQUIRE_DRM_DISPLAY_EXTENSION_NAME,
#ifdef SECOND_DISPLAY_TEST_EXT
        SECOND_DISPLAY_TEST_EXT,
#endif
    };
    uint32_t n = sizeof ext / sizeof *ext;
    if (!have_instance_exts(ext, n)) return false;
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "MHGURecomp second display",
                             .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                               .enabledExtensionCount = n, .ppEnabledExtensionNames = ext};
    VkResult r = vkCreateInstance(&ci, NULL, &s.inst);
    if (r != VK_SUCCESS) { s.inst = VK_NULL_HANDLE; SD_LOG("vkCreateInstance: %d\n", r); return false; }

    uint32_t c = 8;
    VkPhysicalDevice pd[8];
    r = vkEnumeratePhysicalDevices(s.inst, &c, pd);
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) { SD_LOG("vkEnumeratePhysicalDevices: %d\n", r); return false; }
    for (uint32_t i = 0; i < c && !s.phys; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd[i], &p);
        if (p.vendorID == gpu_vendor && p.deviceID == gpu_device) s.phys = pd[i];
    }
    if (!s.phys) SD_LOG("the game's GPU is not in this instance's list\n");
    return s.phys != VK_NULL_HANDLE;
}

// A device of its own, with one queue that can present on the panel.
static bool create_device_for_panel(void) {
    uint32_t n = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(s.phys, &n, NULL);
    VkQueueFamilyProperties *qp = calloc(n ? n : 1, sizeof *qp);
    vkGetPhysicalDeviceQueueFamilyProperties(s.phys, &n, qp);
    uint32_t qfam = UINT32_MAX;
    for (uint32_t i = 0; i < n && qfam == UINT32_MAX; i++) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(s.phys, i, s.surface, &present);
        if (present && (qp[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT))) qfam = i;
    }
    free(qp);
    if (qfam == UINT32_MAX) { SD_LOG("no queue family presents on the panel\n"); return false; }
    float prio = 0.0f;  // (the lowest inside this device: the game's device is apart)
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qfam,
                                  .queueCount = 1, .pQueuePriorities = &prio};
    const char *ext[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi,
                             .enabledExtensionCount = 1, .ppEnabledExtensionNames = ext};
    VkResult r = vkCreateDevice(s.phys, &di, NULL, &s.dev);
    if (r != VK_SUCCESS) { s.dev = VK_NULL_HANDLE; SD_LOG("vkCreateDevice: %d\n", r); return false; }
    vkGetDeviceQueue(s.dev, qfam, 0, &s.queue);
    VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                  .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = qfam};
    if (vkCreateCommandPool(s.dev, &pi, NULL, &s.pool) != VK_SUCCESS) { s.pool = VK_NULL_HANDLE; return false; }
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = s.pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    return vkAllocateCommandBuffers(s.dev, &ai, &s.cb) == VK_SUCCESS;
}

static VkPresentModeKHR panel_present_mode(void) {
    uint32_t n = 8;
    VkPresentModeKHR m[8];
    vkGetPhysicalDeviceSurfacePresentModesKHR(s.phys, s.surface, &n, m);
    for (uint32_t i = 0; i < n; i++) if (m[i] == VK_PRESENT_MODE_MAILBOX_KHR) return m[i];
    return VK_PRESENT_MODE_FIFO_KHR;  // (always there; a full queue then shows as acquire giving nothing: skipped)
}

static bool create_swapchain_for_panel(void) {
    VkSurfaceCapabilitiesKHR caps;
    VkResult r = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(s.phys, s.surface, &caps);
    if (r != VK_SUCCESS) { SD_LOG("surface capabilities: %d\n", r); return false; }
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) { SD_LOG("the panel's images can't be copied to\n"); return false; }
    uint32_t nf = 32;
    VkSurfaceFormatKHR f[32];
    r = vkGetPhysicalDeviceSurfaceFormatsKHR(s.phys, s.surface, &nf, f);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || !nf) { SD_LOG("surface formats: %d\n", r); return false; }
    VkSurfaceFormatKHR sf = f[0];
    for (uint32_t i = 0; i < nf; i++)  // (the bytes as written, as the game's own swapchain: device.c)
        if (f[i].format == VK_FORMAT_B8G8R8A8_UNORM) { sf = f[i]; break; }
    uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    if (count > MAX_IMAGES) count = MAX_IMAGES;
    s.extent = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent : (VkExtent2D){s.out.width, s.out.height};
    s.format = sf.format;
    VkCompositeAlphaFlagBitsKHR alpha = caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
        ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR : (VkCompositeAlphaFlagBitsKHR)(caps.supportedCompositeAlpha & -caps.supportedCompositeAlpha);
    VkPresentModeKHR mode = panel_present_mode();
    VkSwapchainCreateInfoKHR ci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = s.surface, .minImageCount = count,
        .imageFormat = sf.format, .imageColorSpace = sf.colorSpace, .imageExtent = s.extent, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT, .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR, .compositeAlpha = alpha, .presentMode = mode, .clipped = VK_TRUE};
    r = vkCreateSwapchainKHR(s.dev, &ci, NULL, &s.swapchain);
    if (r != VK_SUCCESS) { s.swapchain = VK_NULL_HANDLE; SD_LOG("vkCreateSwapchainKHR: %d\n", r); return false; }
    s.count = MAX_IMAGES;
    r = vkGetSwapchainImagesKHR(s.dev, s.swapchain, &s.count, s.images);
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) { SD_LOG("vkGetSwapchainImagesKHR: %d\n", r); return false; }
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if (vkCreateSemaphore(s.dev, &si, NULL, &s.acquired) != VK_SUCCESS) return false;
    for (uint32_t i = 0; i < s.count; i++)
        if (vkCreateSemaphore(s.dev, &si, NULL, &s.rendered[i]) != VK_SUCCESS) return false;
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
    if (vkCreateFence(s.dev, &fi, NULL, &s.done) != VK_SUCCESS) { s.done = VK_NULL_HANDLE; return false; }
    SD_LOG("swapchain %ux%u, %u images, format %d, present mode %d\n", s.extent.width, s.extent.height, s.count, sf.format, mode);
    return true;
}

static bool create_staging(void) {
    VkDeviceSize size = (VkDeviceSize)s.extent.width * s.extent.height * 4;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                             .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    if (vkCreateBuffer(s.dev, &bi, NULL, &s.staging) != VK_SUCCESS) { s.staging = VK_NULL_HANDLE; return false; }
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(s.dev, s.staging, &mr);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(s.phys, &mp);
    VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if (!(mr.memoryTypeBits & 1u << i) || (mp.memoryTypes[i].propertyFlags & want) != want) continue;
        VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = i};
        if (vkAllocateMemory(s.dev, &ai, NULL, &s.staging_mem) != VK_SUCCESS) { s.staging_mem = VK_NULL_HANDLE; return false; }
        void *p;
        if (vkBindBufferMemory(s.dev, s.staging, s.staging_mem, 0) != VK_SUCCESS ||
            vkMapMemory(s.dev, s.staging_mem, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) return false;
        s.pixels = p;
        return true;
    }
    SD_LOG("no host-visible memory\n");
    return false;
}

static void image_barrier(VkImage img, VkAccessFlags src, VkAccessFlags dst, VkImageLayout from, VkImageLayout to,
                          VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = src, .dstAccessMask = dst,
                              .oldLayout = from, .newLayout = to, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
                              .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(s.cb, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

// One picture onto the panel, or none: anything not ready at once skips this turn. False: the panel is gone.
static bool present_once(void) {
    static SecondDisplayState st;
    static uint32_t *frame;  // (the upright picture second_display_compose makes, turned into the panel's pixels below)
    VkResult r = vkGetFenceStatus(s.dev, s.done);
    if (r == VK_NOT_READY) return true;  // (the last copy is still running)
    if (r != VK_SUCCESS) { SD_LOG("fence: %d\n", r); return false; }
    uint32_t w, h;
    upright_size(&w, &h);
    if (!frame && !(frame = malloc((size_t)w * h * 4))) return true;
    if (!second_display_compose(frame, w, h, &st)) return true;  // (nothing new: the panel keeps its picture)
    bool bgr = s.format == VK_FORMAT_B8G8R8A8_UNORM || s.format == VK_FORMAT_B8G8R8A8_SRGB;
    for (uint32_t v = 0; v < h; v++)
        for (uint32_t u = 0; u < w; u++) {
            uint32_t p = frame[v * w + u];
            put(u, v, 0xff000000u | (bgr ? (p & 0xff00u) | (p >> 16 & 0xffu) | (p & 0xffu) << 16 : p));
        }
    uint32_t img;
    r = vkAcquireNextImageKHR(s.dev, s.swapchain, 0, s.acquired, VK_NULL_HANDLE, &img);
    if (r == VK_NOT_READY || r == VK_TIMEOUT) return true;
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { SD_LOG("acquire: %d\n", r); return false; }
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkResetCommandBuffer(s.cb, 0);
    vkBeginCommandBuffer(s.cb, &bi);
    image_barrier(s.images[img], 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy rg = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                            .imageExtent = {s.extent.width, s.extent.height, 1}};
    vkCmdCopyBufferToImage(s.cb, s.staging, s.images[img], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
    image_barrier(s.images[img], VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    vkEndCommandBuffer(s.cb);
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &s.acquired,
                       .pWaitDstStageMask = &stage, .commandBufferCount = 1, .pCommandBuffers = &s.cb,
                       .signalSemaphoreCount = 1, .pSignalSemaphores = &s.rendered[img]};
    vkResetFences(s.dev, 1, &s.done);
    r = vkQueueSubmit(s.queue, 1, &si, s.done);
    if (r != VK_SUCCESS) { SD_LOG("submit: %d\n", r); return false; }
    VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &s.rendered[img],
                           .swapchainCount = 1, .pSwapchains = &s.swapchain, .pImageIndices = &img};
    r = vkQueuePresentKHR(s.queue, &pi);
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { SD_LOG("present: %d\n", r); return false; }
    return true;
}

static void tear_down(void) {
    g_bottom_hud_on = false;
    if (s.dev) {
        vkDeviceWaitIdle(s.dev);
        if (s.staging) vkDestroyBuffer(s.dev, s.staging, NULL);
        if (s.staging_mem) vkFreeMemory(s.dev, s.staging_mem, NULL);
        if (s.done) vkDestroyFence(s.dev, s.done, NULL);
        for (uint32_t i = 0; i < s.count; i++) if (s.rendered[i]) vkDestroySemaphore(s.dev, s.rendered[i], NULL);
        if (s.acquired) vkDestroySemaphore(s.dev, s.acquired, NULL);
        if (s.pool) vkDestroyCommandPool(s.dev, s.pool, NULL);
        if (s.swapchain) vkDestroySwapchainKHR(s.dev, s.swapchain, NULL);
        vkDestroyDevice(s.dev, NULL);
    }
    if (s.inst) {
        if (s.surface) vkDestroySurfaceKHR(s.inst, s.surface, NULL);
        if (s.phys) drm_lease_release_display(s.phys);
        vkDestroyInstance(s.inst, NULL);
    }
    if (s.have_lease) drm_lease_release();
    memset(&s, 0, sizeof s);
}

// A finger at panel pixel x, y (the touch reader's thread): into the upright picture, as put() turns it out.
void second_display_touch(bool down, unsigned x, unsigned y) {
    uint32_t pw = s.extent.width, ph = s.extent.height, u, v;
    if (!pw || !ph) return;
    switch (s.out.rotation) {
    case 90: u = ph - 1 - y, v = x; break;
    case 180: u = pw - 1 - x, v = ph - 1 - y; break;
    case 270: u = y, v = pw - 1 - x; break;
    default: u = x, v = y; break;
    }
    bottom_hud_touch(down, u, v);
}

static bool bring_up(void) {
    const char *rotation = getenv("MHGU_SECOND_DISPLAY_ROTATION"), *touch = getenv("MHGU_SECOND_DISPLAY_TOUCH");
    if (!touch) touch = "auto";
    if (!strcmp(touch, "off")) touch = NULL;
    if (!drm_lease_acquire(getenv("MHGU_SECOND_DISPLAY_CONNECTOR"), rotation ? atoi(rotation) : -1, touch, &s.out)) return false;
    s.have_lease = true;
    if (!create_instance_for_panel()) return false;
    s.surface = drm_lease_create_surface(s.inst, s.phys, &s.out);
    if (!s.surface || !create_device_for_panel() || !create_swapchain_for_panel() || !create_staging()) return false;
    if (!present_once()) return false;
    uint32_t w, h;
    upright_size(&w, &h);
    bottom_hud_set_size(w, h);
    g_bottom_hud_on = true;
    return true;
}

static void *panel_thread(void *arg) {
    (void)arg;
    // Signals stay with the game's threads, and the scheduler favours them over this one.
    sigset_t set;
    sigfillset(&set);
    sigdelset(&set, SIGSEGV), sigdelset(&set, SIGBUS), sigdelset(&set, SIGILL), sigdelset(&set, SIGFPE), sigdelset(&set, SIGABRT);
    pthread_sigmask(SIG_BLOCK, &set, NULL);
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 5);

    if (!bring_up()) {
        SD_LOG("not available: the game goes on, on its window only\n");
        tear_down();
        return NULL;
    }
    SD_LOG("up: showing the background until the game draws a panel\n");
    const char *e = getenv("MHGU_SECOND_DISPLAY_HZ");
    long hz = e ? atol(e) : 60;
    hz = hz < 1 ? 1 : hz > 240 ? 240 : hz;
    struct timespec period = {.tv_sec = hz == 1, .tv_nsec = hz == 1 ? 0 : 1000000000L / hz};
    while (drm_lease_alive() && present_once())
        nanosleep(&period, NULL);
    SD_LOG("the panel is gone: the game goes on, on its window only\n");
    tear_down();
    return NULL;
}

void second_display_init(bool enabled, uint32_t vendor, uint32_t device) {
    if (bottom_hud_test_init()) return;  // (test: the bottom picture without a panel)
    if (!enabled) return;
    gpu_vendor = vendor, gpu_device = device;
    // (read when the thread's vkCreateInstance runs; the game's instance exists by now, and nothing makes another)
    setenv("VK_LOADER_LAYERS_DISABLE", "~implicit~", 1);
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &attr, panel_thread, NULL) != 0) SD_LOG("no thread: the game goes on, on its window only\n");
    pthread_attr_destroy(&attr);
}
#endif  // MHGU_SECOND_DISPLAY
#endif
