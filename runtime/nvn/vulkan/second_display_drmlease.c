// The bottom screen (bottom_hud.c) on a panel the compositor leases to the game (drm-lease-v1), driven directly through
// VK_KHR_display while the game plays in its normal window. On Armada's gamescope (Game Mode) that is the bottom panel of
// a dual-screen handheld. What it shows is second_display.c's picture, the same as on the other platforms.
//   [Dual Screen] SecondDisplay = On (settings menu, Display tab), Panels, Background, Wallpaper: config.c
// Which backend: at the start the compositor is asked once whether it offers a panel to lease (drm_lease_offered). If it
// does, this file shows the bottom screen there; if not (a desktop session such as Plasma, where both panels are outputs
// of their own), second_display_desktop.c's window on another monitor does (its second_display_tick, built under the
// name second_display_desktop_tick: Makefile).
//   MHGU_SECOND_DISPLAY_CONNECTOR=<name>   that connector (default: the first internal panel offered)
//   MHGU_SECOND_DISPLAY_ROTATION=0|90|180|270   the picture's turn, counter-clockwise (default: the connector's panel
//                                               orientation)
//   MHGU_SECOND_DISPLAY_TOUCH=auto|<path>|<name>|off   the panel's touchscreen, for its panels (bottom_hud.c; default
//                                                  auto). The device is grabbed: the compositor stops seeing it while
//                                                  the panel is leased
//   MHGU_SECOND_DISPLAY_HZ=1..240   how often the picture changes (default 60: the game's own rate)
//
// The game's frame must never wait for this panel, so it shares nothing with the game's Vulkan objects: one thread owns
// a VkInstance and a VkDevice of its own, with their own VkQueue, swapchain, command pool and memory. That thread never
// takes vk_lock; of the game's Vulkan objects it only reads the timeline semaphore's value and host memory the GPU has
// finished writing (bottom_hud_take, through second_display_compose), and the game's thread never waits for it. The
// panel's own vkAcquireNextImageKHR has timeout 0 and a frame that can't start at once is skipped.
// The game's instance and device are created exactly as without this file. Implicit layers stay off the thread's
// instance, so no overlay layer can add a lock shared with the game's present.
// The lease is held while SecondDisplay is On; turning it Off gives the panel back to the compositor.
#define _GNU_SOURCE
#include "../../config.h"
#if defined(MHGU_DUAL_SCREEN) && defined(MHGU_DRM_LEASE)
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#include "../../rt.h"
#include "drm_lease.h"
#include "bottom_hud.h"
#include "second_display.h"
#include "vk.h"

#define SD_LOG(...) LOG("[second-display] " __VA_ARGS__)
#define MAX_IMAGES 8

void second_display_desktop_tick(void);  // (second_display_desktop.c's second_display_tick: Makefile)

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
    uint32_t *frame;        // (the upright picture, second_display_compose's, before it is turned onto the panel)
    SecondDisplayState st;
    bool pending;           // (pixels hold a picture not yet presented)
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

// second_display_compose's upright picture into the panel's pixels: turned, in the swapchain's byte order.
static void turn_onto_panel(uint32_t w, uint32_t h) {
    bool bgr = s.format == VK_FORMAT_B8G8R8A8_UNORM || s.format == VK_FORMAT_B8G8R8A8_SRGB;
    for (uint32_t v = 0; v < h; v++)
        for (uint32_t u = 0; u < w; u++) {
            uint32_t p = s.frame[v * w + u];
            put(u, v, bgr ? (p & 0xff00ff00u) | (p >> 16 & 0xffu) | (p & 0xffu) << 16 : p);
        }
}

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
        if (p.vendorID == g_vk.props.vendorID && p.deviceID == g_vk.props.deviceID) s.phys = pd[i];
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
    VkResult r = vkGetFenceStatus(s.dev, s.done);
    if (r == VK_NOT_READY) return true;  // (the last copy is still running)
    if (r != VK_SUCCESS) { SD_LOG("fence: %d\n", r); return false; }
    uint32_t w, h;
    upright_size(&w, &h);
    if (second_display_compose(s.frame, w, h, &s.st)) turn_onto_panel(w, h), s.pending = true;
    if (!s.pending) return true;
    uint32_t img;
    r = vkAcquireNextImageKHR(s.dev, s.swapchain, 0, s.acquired, VK_NULL_HANDLE, &img);
    if (r == VK_NOT_READY || r == VK_TIMEOUT) return true;  // (pending: presented on a later turn)
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
    s.pending = false;
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
    free(s.frame);
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
    uint32_t w, h;
    upright_size(&w, &h);
    s.frame = malloc((size_t)w * h * 4);
    if (!s.frame || !present_once()) return false;
    bottom_hud_set_size(w, h);
    g_bottom_hud_on = true;
    return true;
}

// ---- the panel's thread, and the UI thread's side
enum { LEASE_UNKNOWN, LEASE_ASKING, LEASE_NONE, LEASE_OFFERED };
static atomic_int lease = LEASE_UNKNOWN;  // (does the compositor offer a panel: asked once, by probe_thread)
static atomic_bool panel_running, panel_stop;

static void *panel_thread(void *arg) {
    (void)arg;
    // Signals stay with the game's threads, and the scheduler favours them over this one.
    sigset_t set;
    sigfillset(&set);
    sigdelset(&set, SIGSEGV), sigdelset(&set, SIGBUS), sigdelset(&set, SIGILL), sigdelset(&set, SIGFPE), sigdelset(&set, SIGABRT);
    pthread_sigmask(SIG_BLOCK, &set, NULL);
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 5);

    if (bring_up()) {
        SD_LOG("up: the panels' HUD parts leave the main screen\n");
        const char *e = getenv("MHGU_SECOND_DISPLAY_HZ");
        long hz = e ? atol(e) : 60;
        hz = hz < 1 ? 1 : hz > 240 ? 240 : hz;
        struct timespec period = {.tv_sec = hz == 1, .tv_nsec = hz == 1 ? 0 : 1000000000L / hz};
        while (!atomic_load(&panel_stop) && drm_lease_alive() && present_once())
            nanosleep(&period, NULL);
        if (atomic_load(&panel_stop)) SD_LOG("off: the HUD is all on the main screen\n");
        else SD_LOG("the panel is gone: the game goes on, on its window only\n");
    } else {
        SD_LOG("not available: the game goes on, on its window only\n");
    }
    tear_down();
    atomic_store(&panel_running, false);
    return NULL;
}

static void start_panel(void) {
    // (read when the thread's vkCreateInstance runs; the game's instance exists by now, and nothing makes another)
    setenv("VK_LOADER_LAYERS_DISABLE", "~implicit~", 1);
    atomic_store(&panel_stop, false);
    atomic_store(&panel_running, true);
    pthread_t th;
    if (pthread_create(&th, NULL, panel_thread, NULL) != 0) {
        atomic_store(&panel_running, false);
        SD_LOG("no thread: the game goes on, on its window only\n");
        return;
    }
    pthread_detach(th);
}

static void *probe_thread(void *arg) {
    (void)arg;
    bool offered = drm_lease_offered(getenv("MHGU_SECOND_DISPLAY_CONNECTOR"));
    atomic_store(&lease, offered ? LEASE_OFFERED : LEASE_NONE);
    return NULL;
}

// The first turn asks the compositor (on a thread: it can take seconds); until it answers, a request to apply waits.
void second_display_tick(void) {
    int l = atomic_load(&lease);
    if (l == LEASE_UNKNOWN) {
        pthread_t th;
        atomic_store(&lease, LEASE_ASKING);
        if (getenv("MHGU_BOTTOM_HUD_TEST") || pthread_create(&th, NULL, probe_thread, NULL) != 0)
            atomic_store(&lease, LEASE_NONE);  // (test: the picture without a panel, as the desktop backend does it)
        else
            pthread_detach(th);
        return;
    }
    if (l == LEASE_ASKING) return;
    if (l == LEASE_NONE) {
        second_display_desktop_tick();
        return;
    }
    if (!config_second_display_available()) {
        config_set_second_display_available(1);
        SD_LOG("the compositor offers a panel to lease: the bottom screen goes there\n");
    }
    if (!config_second_display_take_request()) return;
    if (atomic_load(&panel_running)) {
        if (!g_config.second_display) atomic_store(&panel_stop, true);
        else if (atomic_load(&panel_stop)) config_second_display_request();  // (on again before it was off: once it is)
        return;
    }
    if (g_config.second_display) start_panel();
}
#endif
