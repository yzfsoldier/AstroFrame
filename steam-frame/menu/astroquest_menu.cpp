// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The settings menu shown in the headset before AstroQuest starts on the Steam Frame.
//
//   astroquest-menu <settings.txt>          show the menu; on Start, write the choices and exit 0
//   astroquest-menu --render <out.ppm> [settings.txt]   draw the panel into a file (no headset)
//
// A panel in front of the player (an OpenXR quad layer), drawn here on the processor and copied
// into the layer's image; the controllers' sticks choose a line and change it, A or a trigger
// changes it too, and on "Start" the settings go into settings.txt, where the launcher takes
// them from. Settings set in the game's launch options (ASTROQUEST_<KEY>) win over the file:
// those are shown, and cannot be changed here.
//
// Exit: 0 started, 2 no headset or runtime (the launcher goes on without the menu), 1 an error.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <dlfcn.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "font8x16.h"

namespace {

void Log(const char* format, ...) {
    va_list args;
    va_start(args, format);
    std::vfprintf(stdout, format, args);
    va_end(args);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

// --- the settings -------------------------------------------------------------------------------

struct Choice {
    std::string value;
    std::string shown;
};

struct Item {
    std::string key;
    std::string label;
    std::vector<Choice> choices;
    std::string fallback; ///< what the launcher takes when nothing is set
    std::string help;
    size_t index{};
    bool locked{}; ///< set in the launch options
};

/// What the menu offers; the defaults are the launcher's (astroquest.sh).
std::vector<Item> MakeItems() {
    return {
        {"foveation", "Foveated rendering",
         {{"0", "Off"}, {"1", "Mild"}, {"2", "Medium"}, {"3", "Strong"}}, "3",
         "Coarser shading away from where you look: more GPU time for a larger picture"},
        {"eye_tracking", "Follows your eyes", {{"1", "On"}, {"0", "Off"}}, "1",
         "The sharp middle of foveated rendering goes where you look"},
        {"detail", "Detail further away",
         {{"console", "As on the console"}, {"lighter", "Lighter: faster"}}, "console",
         "Simpler models further away and smaller sun shadow maps"},
        {"fsr", "Picture enlarged with",
         {{"0", "The game's own"}, {"1", "FSR 1 from 1.3x"}, {"2", "FSR 1"}}, "0",
         "Only while the scene is drawn smaller than the eye's picture"},
        {"resolution", "Picture size an eye",
         {{"816", "Fixed 816 x 870"},
          {"960", "Fixed 960 x 1080"},
          {"1200", "Fixed 1200 x 1280"},
          {"1440", "Fixed 1440 x 1536"},
          {"", "Auto, up to 1440 x 1536"},
          {"1600", "Auto, up to 1600 x 1704"},
          {"1800", "Auto, up to 1800 x 1920"},
          {"2016", "Auto, up to 2016 x 2152"},
          {"2160", "Auto, up to 2160 x 2304"}},
         "", "Auto: as large as the GPU manages; above 1440 sharper, more GPU time"},
        {"msaa", "Multisampling", {{"1", "Off: faster"}, {"4", "4x"}}, "1",
         "Off: the emulator smooths the edges itself"},
        {"fov", "Field of view",
         {{"70", "70%"}, {"75", "75%"}, {"80", "80%"}, {"85", "85%"}, {"90", "90%"},
          {"95", "95%"}, {"100", "100%"}},
         "85", "Less is sharper, with a soft border where the rest was"},
        {"sharpen", "Sharpening",
         {{"0", "Off"}, {"0.2", "0.2"}, {"0.4", "0.4"}, {"0.6", "0.6"}, {"0.8", "0.8"}, {"1", "1"}},
         "0.6", "How much every frame of the game is sharpened"},
        {"refresh_rate", "Display refresh rate",
         {{"72", "72 Hz"}, {"90", "90 Hz"}, {"120", "120 Hz"}}, "90",
         "Where SteamVR lets a program choose (it offers 72 Hz for now)"},
        {"overlay", "Debug overlay", {{"0", "Off"}, {"1", "On"}}, "0",
         "Frame rate, picture size, GPU load and the gaze over the picture"},
        {"foveation_colors", "Foveation in colour", {{"0", "Off"}, {"1", "On"}}, "0",
         "Green: every pixel, yellow: 2x2, red: 4x4, a ring where you look"},
        {"menu", "This menu at start", {{"1", "Show"}, {"0", "Skip"}}, "1",
         "Skipped: menu=1 in settings.txt brings it back"},
    };
}

std::string Trim(std::string text) {
    const auto first = text.find_first_not_of(" \t\r");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r");
    return text.substr(first, last - first + 1);
}

std::vector<std::string> ReadLines(const std::string& path) {
    std::vector<std::string> lines;
    std::ifstream in{path};
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(line);
    }
    return lines;
}

/// The value a key has in the file (its last uncommented line), if any.
bool FileValue(const std::vector<std::string>& lines, const std::string& key, std::string& value) {
    bool found = false;
    for (const std::string& line : lines) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const auto equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        if (Trim(line.substr(0, equals)) == key) {
            value = Trim(line.substr(equals + 1));
            found = true;
        }
    }
    return found;
}

void LoadItems(std::vector<Item>& items, const std::string& path) {
    const auto lines = ReadLines(path);
    for (Item& item : items) {
        std::string upper = "ASTROQUEST_" + item.key;
        std::transform(upper.begin(), upper.end(), upper.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        std::string value = item.fallback;
        if (const char* env = std::getenv(upper.c_str()); env != nullptr && env[0] != 0) {
            value = env;
            item.locked = true;
        } else {
            std::string from_file;
            if (FileValue(lines, item.key, from_file) && !from_file.empty()) {
                value = from_file;
            }
        }
        const auto it = std::find_if(item.choices.begin(), item.choices.end(),
                                     [&](const Choice& c) { return c.value == value; });
        if (it != item.choices.end()) {
            item.index = static_cast<size_t>(it - item.choices.begin());
        } else {
            // A value of the player's own: offered as it is.
            item.choices.insert(item.choices.begin(), Choice{value, value});
            item.index = 0;
        }
    }
}

/// Writes the choices into the file: over the key's line where there is one, after its
/// commented-out line where only that is there, at the end otherwise. Everything else stays.
bool SaveItems(const std::vector<Item>& items, const std::string& path) {
    auto lines = ReadLines(path);
    for (const Item& item : items) {
        if (item.locked) {
            continue;
        }
        const std::string line = item.key + "=" + item.choices[item.index].value;
        bool done = false;
        for (std::string& existing : lines) {
            if (existing.empty() || existing[0] == '#') {
                continue;
            }
            const auto equals = existing.find('=');
            if (equals != std::string::npos && Trim(existing.substr(0, equals)) == item.key) {
                existing = line;
                done = true;
            }
        }
        if (done) {
            continue;
        }
        for (size_t i = 0; i < lines.size(); ++i) {
            const std::string& existing = lines[i];
            if (existing.size() > 1 && existing[0] == '#' &&
                existing.compare(1, item.key.size() + 1, item.key + "=") == 0) {
                lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(i) + 1, line);
                done = true;
                break;
            }
        }
        if (!done) {
            lines.push_back(line);
        }
    }
    const std::string temporary = path + ".new";
    {
        std::ofstream out{temporary, std::ios::trunc};
        if (!out) {
            return false;
        }
        for (const std::string& line : lines) {
            out << line << '\n';
        }
        if (!out) {
            return false;
        }
    }
    return std::rename(temporary.c_str(), path.c_str()) == 0;
}

// --- the panel ----------------------------------------------------------------------------------

constexpr uint32_t PanelWidth = 1280;
constexpr uint32_t PanelHeight = 1000;

struct Rgb {
    uint8_t r, g, b;
};

struct Panel {
    std::vector<uint8_t> pixels = std::vector<uint8_t>(PanelWidth * PanelHeight * 4);

    void Fill(int x, int y, int w, int h, Rgb c) {
        for (int j = std::max(y, 0); j < std::min<int>(y + h, PanelHeight); ++j) {
            for (int i = std::max(x, 0); i < std::min<int>(x + w, PanelWidth); ++i) {
                uint8_t* p = &pixels[(static_cast<size_t>(j) * PanelWidth + i) * 4];
                p[0] = c.r;
                p[1] = c.g;
                p[2] = c.b;
                p[3] = 255;
            }
        }
    }

    /// Text at a scale (pixels of the panel for one of the font); returns its width.
    int Text(int x, int y, const std::string& text, int scale, Rgb c) {
        for (size_t n = 0; n < text.size(); ++n) {
            const unsigned ch = static_cast<unsigned char>(text[n]);
            if (ch < 32 || ch > 126) {
                continue;
            }
            for (int row = 0; row < 16; ++row) {
                const uint32_t word = Font8x16[(ch - 32) * 4 + row / 4];
                const uint32_t bits = (word >> ((row % 4) * 8)) & 0xFF;
                for (int col = 0; col < 8; ++col) {
                    if ((bits >> col) & 1) {
                        Fill(x + static_cast<int>(n) * 8 * scale + col * scale, y + row * scale,
                             scale, scale, c);
                    }
                }
            }
        }
        return static_cast<int>(text.size()) * 8 * scale;
    }
};

constexpr Rgb Background{18, 22, 30};
constexpr Rgb Title{255, 255, 255};
constexpr Rgb Label{205, 212, 225};
constexpr Rgb Value{255, 214, 102};
constexpr Rgb Locked{140, 146, 160};
constexpr Rgb Selected{44, 72, 120};
constexpr Rgb Help{160, 170, 190};
constexpr Rgb StartColor{120, 220, 140};

void Draw(Panel& panel, const std::vector<Item>& items, size_t selected, const std::string& note) {
    panel.Fill(0, 0, PanelWidth, PanelHeight, Background);
    panel.Fill(0, 0, PanelWidth, 6, Value);
    panel.Text(48, 30, "AstroQuest - Steam Frame", 3, Title);
    panel.Text(48, 86, "Settings for this start", 2, Help);

    constexpr int top = 138;
    constexpr int row_height = 56;
    for (size_t i = 0; i <= items.size(); ++i) {
        const int y = top + static_cast<int>(i) * row_height;
        if (i == selected) {
            panel.Fill(24, y - 6, PanelWidth - 48, row_height - 4, Selected);
        }
        if (i == items.size()) {
            panel.Text(48, y, "Start", 3, StartColor);
            continue;
        }
        const Item& item = items[i];
        panel.Text(48, y + 4, item.label, 2, item.locked ? Locked : Label);
        const std::string shown = item.choices[item.index].shown;
        const std::string value =
            item.locked ? shown + " (launch options)" : "< " + shown + " >";
        panel.Text(620, y + 4, value, 2, item.locked ? Locked : Value);
    }
    const int help_y = top + static_cast<int>(items.size() + 1) * row_height + 10;
    const std::string help = !note.empty()               ? note
                             : selected < items.size() ? items[selected].help
                                                       : "Saves these settings and starts the game";
    panel.Text(48, help_y, help, 2, Help);
    panel.Text(48, help_y + 44, "Stick up/down: choose   left/right or A/trigger: change", 2, Help);
}

bool WritePpm(const Panel& panel, const std::string& path) {
    std::ofstream out{path, std::ios::binary};
    out << "P6\n" << PanelWidth << ' ' << PanelHeight << "\n255\n";
    for (size_t i = 0; i < panel.pixels.size(); i += 4) {
        out.put(static_cast<char>(panel.pixels[i]));
        out.put(static_cast<char>(panel.pixels[i + 1]));
        out.put(static_cast<char>(panel.pixels[i + 2]));
    }
    return static_cast<bool>(out);
}

// --- Vulkan, as little as copying a picture into the layer takes ------------------------------

struct Vk {
    void* library{};
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr{};
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    uint32_t family{};
    VkQueue queue{};
    VkCommandPool pool{};
    VkCommandBuffer cmd{};
    VkFence fence{};
    VkBuffer staging{};
    VkDeviceMemory staging_memory{};
    void* mapped{};

#define VK_FN(name) PFN_vk##name name{}
    VK_FN(GetPhysicalDeviceQueueFamilyProperties);
    VK_FN(GetPhysicalDeviceMemoryProperties);
    VK_FN(GetDeviceProcAddr);
    VK_FN(DestroyInstance);
    VK_FN(GetDeviceQueue);
    VK_FN(CreateCommandPool);
    VK_FN(DestroyCommandPool);
    VK_FN(AllocateCommandBuffers);
    VK_FN(BeginCommandBuffer);
    VK_FN(EndCommandBuffer);
    VK_FN(ResetCommandBuffer);
    VK_FN(CmdPipelineBarrier);
    VK_FN(CmdCopyBufferToImage);
    VK_FN(QueueSubmit);
    VK_FN(CreateFence);
    VK_FN(DestroyFence);
    VK_FN(WaitForFences);
    VK_FN(ResetFences);
    VK_FN(CreateBuffer);
    VK_FN(DestroyBuffer);
    VK_FN(GetBufferMemoryRequirements);
    VK_FN(AllocateMemory);
    VK_FN(FreeMemory);
    VK_FN(BindBufferMemory);
    VK_FN(MapMemory);
    VK_FN(DeviceWaitIdle);
    VK_FN(DestroyDevice);
#undef VK_FN

    bool Open() {
        for (const char* name : {"libvulkan.so.1", "libvulkan.so"}) {
            library = dlopen(name, RTLD_NOW | RTLD_LOCAL);
            if (library != nullptr) {
                break;
            }
        }
        if (library == nullptr) {
            Log("No Vulkan loader (libvulkan.so.1)");
            return false;
        }
        GetInstanceProcAddr =
            reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(library, "vkGetInstanceProcAddr"));
        return GetInstanceProcAddr != nullptr;
    }

    void LoadInstance() {
#define VK_LOAD(name) name = reinterpret_cast<PFN_vk##name>(GetInstanceProcAddr(instance, "vk" #name))
        VK_LOAD(GetPhysicalDeviceQueueFamilyProperties);
        VK_LOAD(GetPhysicalDeviceMemoryProperties);
        VK_LOAD(GetDeviceProcAddr);
        VK_LOAD(DestroyInstance);
#undef VK_LOAD
    }

    bool LoadDevice() {
#define VK_LOAD(name)                                                                            \
    name = reinterpret_cast<PFN_vk##name>(GetDeviceProcAddr(device, "vk" #name));                \
    if (name == nullptr) {                                                                       \
        Log("Vulkan: no vk" #name);                                                              \
        return false;                                                                            \
    }
        VK_LOAD(GetDeviceQueue);
        VK_LOAD(CreateCommandPool);
        VK_LOAD(DestroyCommandPool);
        VK_LOAD(AllocateCommandBuffers);
        VK_LOAD(BeginCommandBuffer);
        VK_LOAD(EndCommandBuffer);
        VK_LOAD(ResetCommandBuffer);
        VK_LOAD(CmdPipelineBarrier);
        VK_LOAD(CmdCopyBufferToImage);
        VK_LOAD(QueueSubmit);
        VK_LOAD(CreateFence);
        VK_LOAD(DestroyFence);
        VK_LOAD(WaitForFences);
        VK_LOAD(ResetFences);
        VK_LOAD(CreateBuffer);
        VK_LOAD(DestroyBuffer);
        VK_LOAD(GetBufferMemoryRequirements);
        VK_LOAD(AllocateMemory);
        VK_LOAD(FreeMemory);
        VK_LOAD(BindBufferMemory);
        VK_LOAD(MapMemory);
        VK_LOAD(DeviceWaitIdle);
        VK_LOAD(DestroyDevice);
#undef VK_LOAD
        return true;
    }

    bool MakeResources(VkDeviceSize size) {
        GetDeviceQueue(device, family, 0, &queue);
        VkCommandPoolCreateInfo pool_ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool_ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_ci.queueFamilyIndex = family;
        if (CreateCommandPool(device, &pool_ci, nullptr, &pool) != VK_SUCCESS) {
            return false;
        }
        VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        alloc.commandPool = pool;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        if (AllocateCommandBuffers(device, &alloc, &cmd) != VK_SUCCESS) {
            return false;
        }
        VkFenceCreateInfo fence_ci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (CreateFence(device, &fence_ci, nullptr, &fence) != VK_SUCCESS) {
            return false;
        }
        VkBufferCreateInfo buffer_ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer_ci.size = size;
        buffer_ci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        if (CreateBuffer(device, &buffer_ci, nullptr, &staging) != VK_SUCCESS) {
            return false;
        }
        VkMemoryRequirements needs{};
        GetBufferMemoryRequirements(device, staging, &needs);
        VkPhysicalDeviceMemoryProperties memory{};
        GetPhysicalDeviceMemoryProperties(physical, &memory);
        uint32_t type = UINT32_MAX;
        const VkMemoryPropertyFlags wanted =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
            if ((needs.memoryTypeBits & (1u << i)) &&
                (memory.memoryTypes[i].propertyFlags & wanted) == wanted) {
                type = i;
                break;
            }
        }
        if (type == UINT32_MAX) {
            return false;
        }
        VkMemoryAllocateInfo memory_ci{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        memory_ci.allocationSize = needs.size;
        memory_ci.memoryTypeIndex = type;
        if (AllocateMemory(device, &memory_ci, nullptr, &staging_memory) != VK_SUCCESS ||
            BindBufferMemory(device, staging, staging_memory, 0) != VK_SUCCESS ||
            MapMemory(device, staging_memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
            return false;
        }
        return true;
    }

    /// Copies the staging buffer into a swapchain image, which the compositor then takes in
    /// the colour attachment layout.
    bool Upload(VkImage image) {
        ResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        BeginCommandBuffer(cmd, &begin);
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           0, 0, nullptr, 0, nullptr, 1, &barrier);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {PanelWidth, PanelHeight, 1};
        CmdCopyBufferToImage(cmd, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr,
                           1, &barrier);
        EndCommandBuffer(cmd);
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        ResetFences(device, 1, &fence);
        if (QueueSubmit(queue, 1, &submit, fence) != VK_SUCCESS) {
            return false;
        }
        return WaitForFences(device, 1, &fence, VK_TRUE, 1'000'000'000) == VK_SUCCESS;
    }

    void Close() {
        if (device != VK_NULL_HANDLE) {
            DeviceWaitIdle(device);
            if (staging != VK_NULL_HANDLE) {
                DestroyBuffer(device, staging, nullptr);
            }
            if (staging_memory != VK_NULL_HANDLE) {
                FreeMemory(device, staging_memory, nullptr);
            }
            if (fence != VK_NULL_HANDLE) {
                DestroyFence(device, fence, nullptr);
            }
            if (pool != VK_NULL_HANDLE) {
                DestroyCommandPool(device, pool, nullptr);
            }
            DestroyDevice(device, nullptr);
        }
        if (instance != VK_NULL_HANDLE && DestroyInstance != nullptr) {
            DestroyInstance(instance, nullptr);
        }
    }
};

// --- OpenXR ---------------------------------------------------------------------------------------

template <typename T>
T XrFunction(XrInstance instance, const char* name) {
    PFN_xrVoidFunction function = nullptr;
    xrGetInstanceProcAddr(instance, name, &function);
    return reinterpret_cast<T>(function);
}

XrPath Path(XrInstance instance, const char* text) {
    XrPath path = XR_NULL_PATH;
    xrStringToPath(instance, text, &path);
    return path;
}

struct Input {
    XrActionSet set{};
    XrAction move{};
    XrAction press{};
    std::array<XrPath, 2> hands{};
    // What the sticks and buttons did last frame, for edges and repeats.
    int last_y{};
    int last_x{};
    bool last_press{};
    std::chrono::steady_clock::time_point repeat_at{};
};

bool MakeInput(XrInstance instance, XrSession session, Input& input) {
    XrActionSetCreateInfo set_ci{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strcpy(set_ci.actionSetName, "menu");
    std::strcpy(set_ci.localizedActionSetName, "Menu");
    if (XR_FAILED(xrCreateActionSet(instance, &set_ci, &input.set))) {
        return false;
    }
    input.hands = {Path(instance, "/user/hand/left"), Path(instance, "/user/hand/right")};
    XrActionCreateInfo action_ci{XR_TYPE_ACTION_CREATE_INFO};
    action_ci.countSubactionPaths = 2;
    action_ci.subactionPaths = input.hands.data();
    action_ci.actionType = XR_ACTION_TYPE_VECTOR2F_INPUT;
    std::strcpy(action_ci.actionName, "move");
    std::strcpy(action_ci.localizedActionName, "Choose");
    if (XR_FAILED(xrCreateAction(input.set, &action_ci, &input.move))) {
        return false;
    }
    action_ci.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    std::strcpy(action_ci.actionName, "press");
    std::strcpy(action_ci.localizedActionName, "Change");
    if (XR_FAILED(xrCreateAction(input.set, &action_ci, &input.press))) {
        return false;
    }
    struct Profile {
        const char* name;
        std::vector<std::pair<XrAction, const char*>> bindings;
    };
    const std::vector<Profile> profiles = {
        {"/interaction_profiles/oculus/touch_controller",
         {{input.move, "/user/hand/left/input/thumbstick"},
          {input.move, "/user/hand/right/input/thumbstick"},
          {input.press, "/user/hand/right/input/a/click"},
          {input.press, "/user/hand/left/input/x/click"},
          {input.press, "/user/hand/right/input/trigger/value"},
          {input.press, "/user/hand/left/input/trigger/value"}}},
        {"/interaction_profiles/valve/index_controller",
         {{input.move, "/user/hand/left/input/thumbstick"},
          {input.move, "/user/hand/right/input/thumbstick"},
          {input.press, "/user/hand/right/input/a/click"},
          {input.press, "/user/hand/left/input/a/click"},
          {input.press, "/user/hand/right/input/trigger/click"},
          {input.press, "/user/hand/left/input/trigger/click"}}},
        {"/interaction_profiles/khr/simple_controller",
         {{input.press, "/user/hand/right/input/select/click"},
          {input.press, "/user/hand/left/input/select/click"}}},
    };
    for (const Profile& profile : profiles) {
        std::vector<XrActionSuggestedBinding> bindings;
        for (const auto& [action, path] : profile.bindings) {
            bindings.push_back({action, Path(instance, path)});
        }
        XrInteractionProfileSuggestedBinding suggested{
            XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        suggested.interactionProfile = Path(instance, profile.name);
        suggested.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
        suggested.suggestedBindings = bindings.data();
        if (XR_FAILED(xrSuggestInteractionProfileBindings(instance, &suggested))) {
            Log("The runtime takes no bindings for %s", profile.name);
        }
    }
    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets = &input.set;
    return XR_SUCCEEDED(xrAttachSessionActionSets(session, &attach));
}

/// What the player did this frame: -1/+1 up or down, -1/+1 left or right, a press.
struct Moves {
    int line{};
    int change{};
    bool press{};
    bool any{};
};

Moves ReadInput(XrSession session, Input& input) {
    Moves moves{};
    const XrActiveActionSet active{input.set, XR_NULL_PATH};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    if (xrSyncActions(session, &sync) != XR_SUCCESS) {
        return moves;
    }
    float x = 0.0f;
    float y = 0.0f;
    for (const XrPath hand : input.hands) {
        XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
        get.action = input.move;
        get.subactionPath = hand;
        XrActionStateVector2f state{XR_TYPE_ACTION_STATE_VECTOR2F};
        if (XR_SUCCEEDED(xrGetActionStateVector2f(session, &get, &state)) && state.isActive) {
            if (std::abs(state.currentState.x) > std::abs(x)) {
                x = state.currentState.x;
            }
            if (std::abs(state.currentState.y) > std::abs(y)) {
                y = state.currentState.y;
            }
        }
    }
    XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
    get.action = input.press;
    XrActionStateBoolean pressed{XR_TYPE_ACTION_STATE_BOOLEAN};
    const bool press = XR_SUCCEEDED(xrGetActionStateBoolean(session, &get, &pressed)) &&
                       pressed.isActive && pressed.currentState;

    // A stick pushed past two thirds counts once, and again every so often while held.
    const int now_y = y > 0.66f ? -1 : y < -0.66f ? 1 : 0; // up is a line before
    const int now_x = x > 0.66f ? 1 : x < -0.66f ? -1 : 0;
    const auto now = std::chrono::steady_clock::now();
    const bool fresh = now_y != input.last_y || now_x != input.last_x;
    if ((now_y != 0 || now_x != 0) && (fresh || now >= input.repeat_at)) {
        moves.line = now_y;
        moves.change = now_y == 0 ? now_x : 0;
        input.repeat_at = now + std::chrono::milliseconds(fresh ? 450 : 180);
    }
    input.last_y = now_y;
    input.last_x = now_x;
    moves.press = press && !input.last_press;
    input.last_press = press;
    moves.any = moves.line != 0 || moves.change != 0 || moves.press;
    return moves;
}

int RunMenu(const std::string& settings_path) {
    std::vector<Item> items = MakeItems();
    LoadItems(items, settings_path);

    Vk vk;
    if (!vk.Open()) {
        return 2;
    }

    // The instance, with what is needed to hand the runtime Vulkan images.
    uint32_t count = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr);
    std::vector<XrExtensionProperties> offered(count, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, count, &count, offered.data());
    const bool has_enable2 = std::any_of(offered.begin(), offered.end(), [](const auto& e) {
        return std::strcmp(e.extensionName, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME) == 0;
    });
    if (!has_enable2) {
        Log("No OpenXR runtime with %s", XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
        return 2;
    }
    const char* extensions[] = {XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
    XrInstanceCreateInfo instance_ci{XR_TYPE_INSTANCE_CREATE_INFO};
    std::strcpy(instance_ci.applicationInfo.applicationName, "AstroQuest menu");
    std::strcpy(instance_ci.applicationInfo.engineName, "AstroQuest");
    instance_ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    instance_ci.enabledExtensionCount = 1;
    instance_ci.enabledExtensionNames = extensions;
    XrInstance instance{};
    if (XR_FAILED(xrCreateInstance(&instance_ci, &instance))) {
        Log("No OpenXR runtime");
        return 2;
    }
    XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
    system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId system{};
    for (int tries = 0; XR_FAILED(xrGetSystem(instance, &system_info, &system)); ++tries) {
        if (tries == 20) {
            Log("No headset");
            xrDestroyInstance(instance);
            return 2;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    const auto get_requirements = XrFunction<PFN_xrGetVulkanGraphicsRequirements2KHR>(
        instance, "xrGetVulkanGraphicsRequirements2KHR");
    const auto create_instance =
        XrFunction<PFN_xrCreateVulkanInstanceKHR>(instance, "xrCreateVulkanInstanceKHR");
    const auto get_device =
        XrFunction<PFN_xrGetVulkanGraphicsDevice2KHR>(instance, "xrGetVulkanGraphicsDevice2KHR");
    const auto create_device =
        XrFunction<PFN_xrCreateVulkanDeviceKHR>(instance, "xrCreateVulkanDeviceKHR");
    if (!get_requirements || !create_instance || !get_device || !create_device) {
        Log("The runtime lacks the Vulkan functions it offers");
        xrDestroyInstance(instance);
        return 2;
    }
    XrGraphicsRequirementsVulkan2KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
    get_requirements(instance, system, &requirements);

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "AstroQuest menu";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo vk_instance_ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    vk_instance_ci.pApplicationInfo = &app;
    XrVulkanInstanceCreateInfoKHR xr_vk_instance_ci{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
    xr_vk_instance_ci.systemId = system;
    xr_vk_instance_ci.pfnGetInstanceProcAddr = vk.GetInstanceProcAddr;
    xr_vk_instance_ci.vulkanCreateInfo = &vk_instance_ci;
    VkResult vk_result{};
    if (XR_FAILED(create_instance(instance, &xr_vk_instance_ci, &vk.instance, &vk_result)) ||
        vk_result != VK_SUCCESS) {
        Log("Vulkan instance not made (%d)", vk_result);
        xrDestroyInstance(instance);
        return 1;
    }
    vk.LoadInstance();
    XrVulkanGraphicsDeviceGetInfoKHR device_info{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
    device_info.systemId = system;
    device_info.vulkanInstance = vk.instance;
    get_device(instance, &device_info, &vk.physical);
    uint32_t families = 0;
    vk.GetPhysicalDeviceQueueFamilyProperties(vk.physical, &families, nullptr);
    std::vector<VkQueueFamilyProperties> family_props(families);
    vk.GetPhysicalDeviceQueueFamilyProperties(vk.physical, &families, family_props.data());
    for (uint32_t i = 0; i < families; ++i) {
        if (family_props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            vk.family = i;
            break;
        }
    }
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_ci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_ci.queueFamilyIndex = vk.family;
    queue_ci.queueCount = 1;
    queue_ci.pQueuePriorities = &priority;
    VkDeviceCreateInfo vk_device_ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    vk_device_ci.queueCreateInfoCount = 1;
    vk_device_ci.pQueueCreateInfos = &queue_ci;
    XrVulkanDeviceCreateInfoKHR xr_vk_device_ci{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
    xr_vk_device_ci.systemId = system;
    xr_vk_device_ci.pfnGetInstanceProcAddr = vk.GetInstanceProcAddr;
    xr_vk_device_ci.vulkanPhysicalDevice = vk.physical;
    xr_vk_device_ci.vulkanCreateInfo = &vk_device_ci;
    if (XR_FAILED(create_device(instance, &xr_vk_device_ci, &vk.device, &vk_result)) ||
        vk_result != VK_SUCCESS || !vk.LoadDevice() ||
        !vk.MakeResources(VkDeviceSize{PanelWidth} * PanelHeight * 4)) {
        Log("Vulkan device not made (%d)", vk_result);
        vk.Close();
        xrDestroyInstance(instance);
        return 1;
    }

    XrGraphicsBindingVulkan2KHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
    binding.instance = vk.instance;
    binding.physicalDevice = vk.physical;
    binding.device = vk.device;
    binding.queueFamilyIndex = vk.family;
    binding.queueIndex = 0;
    XrSessionCreateInfo session_ci{XR_TYPE_SESSION_CREATE_INFO};
    session_ci.next = &binding;
    session_ci.systemId = system;
    XrSession session{};
    if (XR_FAILED(xrCreateSession(instance, &session_ci, &session))) {
        Log("No session");
        vk.Close();
        xrDestroyInstance(instance);
        return 2;
    }
    XrReferenceSpaceCreateInfo space_ci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    space_ci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    space_ci.poseInReferenceSpace.orientation.w = 1.0f;
    XrSpace space{};
    xrCreateReferenceSpace(session, &space_ci, &space);

    // The panel's image: an sRGB format, the colours above are what it holds.
    uint32_t format_count = 0;
    xrEnumerateSwapchainFormats(session, 0, &format_count, nullptr);
    std::vector<int64_t> formats(format_count);
    xrEnumerateSwapchainFormats(session, format_count, &format_count, formats.data());
    int64_t format = 0;
    bool bgra = false;
    for (const int64_t wanted : {int64_t{VK_FORMAT_R8G8B8A8_SRGB}, int64_t{VK_FORMAT_B8G8R8A8_SRGB},
                                 int64_t{VK_FORMAT_R8G8B8A8_UNORM},
                                 int64_t{VK_FORMAT_B8G8R8A8_UNORM}}) {
        if (std::find(formats.begin(), formats.end(), wanted) != formats.end()) {
            format = wanted;
            bgra = wanted == VK_FORMAT_B8G8R8A8_SRGB || wanted == VK_FORMAT_B8G8R8A8_UNORM;
            break;
        }
    }
    if (format == 0) {
        Log("No image format for the panel");
        return 1;
    }
    XrSwapchainCreateInfo swapchain_ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    swapchain_ci.usageFlags =
        XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    swapchain_ci.format = format;
    swapchain_ci.sampleCount = 1;
    swapchain_ci.width = PanelWidth;
    swapchain_ci.height = PanelHeight;
    swapchain_ci.faceCount = 1;
    swapchain_ci.arraySize = 1;
    swapchain_ci.mipCount = 1;
    XrSwapchain swapchain{};
    if (XR_FAILED(xrCreateSwapchain(session, &swapchain_ci, &swapchain))) {
        Log("No swapchain for the panel");
        return 1;
    }
    uint32_t image_count = 0;
    xrEnumerateSwapchainImages(swapchain, 0, &image_count, nullptr);
    std::vector<XrSwapchainImageVulkan2KHR> images(image_count,
                                                   {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
    xrEnumerateSwapchainImages(swapchain, image_count, &image_count,
                               reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));

    Input input;
    if (!MakeInput(instance, session, input)) {
        Log("No controller input for the menu");
    }

    Panel panel;
    size_t selected = items.size(); // on Start
    bool dirty = true;
    bool started = false;
    bool running = false;
    bool done = false;
    std::string note;
    const auto began = std::chrono::steady_clock::now();
    while (!done) {
        XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
        while (xrPollEvent(instance, &event) == XR_SUCCESS) {
            if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                const auto& changed = reinterpret_cast<XrEventDataSessionStateChanged&>(event);
                if (changed.state == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
                    begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    running = XR_SUCCEEDED(xrBeginSession(session, &begin));
                } else if (changed.state == XR_SESSION_STATE_STOPPING) {
                    xrEndSession(session);
                    running = false;
                } else if (changed.state == XR_SESSION_STATE_EXITING ||
                           changed.state == XR_SESSION_STATE_LOSS_PENDING) {
                    done = true;
                }
            } else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
                done = true;
            }
            event = {XR_TYPE_EVENT_DATA_BUFFER};
        }
        if (done) {
            break;
        }
        if (!running) {
            if (!started && std::chrono::steady_clock::now() - began > std::chrono::seconds(20)) {
                Log("The session never began");
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        XrFrameState frame{XR_TYPE_FRAME_STATE};
        if (XR_FAILED(xrWaitFrame(session, nullptr, &frame))) {
            break;
        }
        xrBeginFrame(session, nullptr);

        if (!started && input.set != XR_NULL_HANDLE) {
            const Moves moves = ReadInput(session, input);
            if (moves.line != 0) {
                selected = (selected + items.size() + 1 + moves.line) % (items.size() + 1);
                note.clear();
            }
            const int change = moves.change != 0 ? moves.change : moves.press ? 1 : 0;
            if (change != 0 && selected < items.size()) {
                Item& item = items[selected];
                if (item.locked) {
                    note = "Set in the game's launch options: change it there";
                } else {
                    const size_t n = item.choices.size();
                    item.index = (item.index + n + change) % n;
                }
            } else if (moves.press && selected == items.size()) {
                if (SaveItems(items, settings_path)) {
                    Log("Settings saved to %s", settings_path.c_str());
                } else {
                    Log("Could not write %s: started with the choices of before",
                        settings_path.c_str());
                }
                started = true;
                note = "Starting...";
                xrRequestExitSession(session);
            }
            dirty |= moves.any;
        }

        if (dirty) {
            Draw(panel, items, selected, note);
            auto* out = static_cast<uint8_t*>(vk.mapped);
            std::memcpy(out, panel.pixels.data(), panel.pixels.size());
            if (bgra) {
                for (size_t i = 0; i < panel.pixels.size(); i += 4) {
                    std::swap(out[i], out[i + 2]);
                }
            }
        }
        std::vector<const XrCompositionLayerBaseHeader*> layers;
        XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
        if (frame.shouldRender) {
            uint32_t index = 0;
            XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wait.timeout = XR_INFINITE_DURATION;
            if (XR_SUCCEEDED(xrAcquireSwapchainImage(swapchain, &acquire, &index)) &&
                XR_SUCCEEDED(xrWaitSwapchainImage(swapchain, &wait))) {
                vk.Upload(images[index].image);
                XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                xrReleaseSwapchainImage(swapchain, &release);
                dirty = false;
                quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                quad.space = space;
                quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                quad.subImage.swapchain = swapchain;
                quad.subImage.imageRect = {{0, 0},
                                           {static_cast<int32_t>(PanelWidth),
                                            static_cast<int32_t>(PanelHeight)}};
                // 1.28 by 1.0 metres, 1.5 metres ahead, a little below the eyes.
                quad.pose.orientation.w = 1.0f;
                quad.pose.position = {0.0f, -0.1f, -1.5f};
                quad.size = {1.28f, 1.0f};
                layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad));
            }
        }
        XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
        end.displayTime = frame.predictedDisplayTime;
        end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        end.layerCount = static_cast<uint32_t>(layers.size());
        end.layers = layers.data();
        xrEndFrame(session, &end);
    }

    xrDestroySwapchain(swapchain);
    xrDestroySpace(space);
    xrDestroySession(session);
    vk.Close();
    xrDestroyInstance(instance);
    return started ? 0 : 2;
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && std::strcmp(argv[1], "--render") == 0) {
        std::vector<Item> items = MakeItems();
        if (argc >= 4) {
            LoadItems(items, argv[3]);
        } else {
            LoadItems(items, "");
        }
        Panel panel;
        Draw(panel, items, 0, "");
        return WritePpm(panel, argv[2]) ? 0 : 1;
    }
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <settings.txt> | --render <out.ppm> [settings.txt]\n",
                     argv[0]);
        return 1;
    }
    return RunMenu(argv[1]);
}
