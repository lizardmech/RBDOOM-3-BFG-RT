#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <vulkan/vulkan.hpp>
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

#include <nvrhi/nvrhi.h>
#include <nvrhi/vulkan.h>

#include "upt00_abi.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
constexpr uint32_t kThreads = 64;
constexpr uint32_t kElementCount = 262144;
constexpr uint32_t kWarmupDispatchCount = 128;
constexpr uint32_t kDispatchCount = 1024;

void checkVk(VkResult result, const char* operation)
{
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + " failed with VkResult " + std::to_string(result));
}

class MessageCallback final : public nvrhi::IMessageCallback
{
public:
    void message(nvrhi::MessageSeverity severity, const char* text) override
    {
        std::cerr << "NVRHI[" << int(severity) << "]: " << text << '\n';
        if (severity == nvrhi::MessageSeverity::Error)
            sawError = true;
    }
    bool sawError = false;
};

struct VulkanContext
{
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkPhysicalDeviceProperties properties{};
    MessageCallback messages;
    nvrhi::vulkan::DeviceHandle nvrhiDevice;

    ~VulkanContext()
    {
        nvrhiDevice = nullptr;
        if (device) vkDestroyDevice(device, nullptr);
        if (instance) vkDestroyInstance(instance, nullptr);
    }
};

bool hasExtension(VkPhysicalDevice physicalDevice, const char* wanted)
{
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> properties(count);
    vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, properties.data());
    return std::any_of(properties.begin(), properties.end(), [wanted](const auto& property) {
        return std::strcmp(property.extensionName, wanted) == 0;
    });
}

std::unique_ptr<VulkanContext> createContext()
{
    auto context = std::make_unique<VulkanContext>();
    VULKAN_HPP_DEFAULT_DISPATCHER.init(vkGetInstanceProcAddr);

    VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "rbdoom UPT-00 compiler A/B";
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo instanceInfo{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    instanceInfo.pApplicationInfo = &app;
    checkVk(vkCreateInstance(&instanceInfo, nullptr, &context->instance), "vkCreateInstance");
    VULKAN_HPP_DEFAULT_DISPATCHER.init(context->instance, vkGetInstanceProcAddr);

    uint32_t physicalDeviceCount = 0;
    checkVk(vkEnumeratePhysicalDevices(context->instance, &physicalDeviceCount, nullptr), "vkEnumeratePhysicalDevices(count)");
    std::vector<VkPhysicalDevice> physicalDevices(physicalDeviceCount);
    checkVk(vkEnumeratePhysicalDevices(context->instance, &physicalDeviceCount, physicalDevices.data()), "vkEnumeratePhysicalDevices");

    constexpr std::array requiredExtensions = {
        VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
        VK_KHR_RAY_QUERY_EXTENSION_NAME,
        VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME
    };

    for (VkPhysicalDevice candidate : physicalDevices)
    {
        if (!std::all_of(requiredExtensions.begin(), requiredExtensions.end(), [candidate](const char* extension) {
            return hasExtension(candidate, extension);
        })) continue;

        VkPhysicalDeviceRayQueryFeaturesKHR rayQuery{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR };
        VkPhysicalDeviceAccelerationStructureFeaturesKHR accelerationStructure{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR };
        VkPhysicalDeviceVulkan12Features vulkan12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
        VkPhysicalDeviceFeatures2 features{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
        features.pNext = &vulkan12;
        vulkan12.pNext = &accelerationStructure;
        accelerationStructure.pNext = &rayQuery;
        vkGetPhysicalDeviceFeatures2(candidate, &features);
        if (!vulkan12.bufferDeviceAddress || !vulkan12.scalarBlockLayout ||
            !accelerationStructure.accelerationStructure || !rayQuery.rayQuery) continue;

        uint32_t queueCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queueCount, nullptr);
        std::vector<VkQueueFamilyProperties> queues(queueCount);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queueCount, queues.data());
        for (uint32_t queueIndex = 0; queueIndex < queueCount; ++queueIndex)
        {
            if ((queues[queueIndex].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
                (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))
            {
                context->physicalDevice = candidate;
                context->queueFamily = queueIndex;
                break;
            }
        }
        if (context->physicalDevice) break;
    }
    if (!context->physicalDevice)
        throw std::runtime_error("No Vulkan 1.2 device supports acceleration structures, ray query, scalar layout, and buffer device address");

    vkGetPhysicalDeviceProperties(context->physicalDevice, &context->properties);
    float priority = 1.f;
    VkDeviceQueueCreateInfo queueInfo{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    queueInfo.queueFamilyIndex = context->queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;

    VkPhysicalDeviceRayQueryFeaturesKHR rayQuery{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR };
    rayQuery.rayQuery = VK_TRUE;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR accelerationStructure{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR };
    accelerationStructure.accelerationStructure = VK_TRUE;
    accelerationStructure.pNext = &rayQuery;
    VkPhysicalDeviceVulkan12Features vulkan12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    vulkan12.bufferDeviceAddress = VK_TRUE;
    vulkan12.scalarBlockLayout = VK_TRUE;
    vulkan12.pNext = &accelerationStructure;

    std::array<const char*, 3> extensions = {
        VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
        VK_KHR_RAY_QUERY_EXTENSION_NAME,
        VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME
    };
    VkDeviceCreateInfo deviceInfo{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    deviceInfo.pNext = &vulkan12;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = uint32_t(extensions.size());
    deviceInfo.ppEnabledExtensionNames = extensions.data();
    checkVk(vkCreateDevice(context->physicalDevice, &deviceInfo, nullptr, &context->device), "vkCreateDevice");
    VULKAN_HPP_DEFAULT_DISPATCHER.init(context->instance, vkGetInstanceProcAddr, context->device);
    vkGetDeviceQueue(context->device, context->queueFamily, 0, &context->queue);

    nvrhi::vulkan::DeviceDesc desc{};
    desc.errorCB = &context->messages;
    desc.instance = context->instance;
    desc.physicalDevice = context->physicalDevice;
    desc.device = context->device;
    desc.graphicsQueue = context->queue;
    desc.graphicsQueueIndex = int(context->queueFamily);
    desc.deviceExtensions = extensions.data();
    desc.numDeviceExtensions = extensions.size();
    desc.bufferDeviceAddressSupported = true;
    desc.maxTimerQueries = 64;
    context->nvrhiDevice = nvrhi::vulkan::createDevice(desc);
    if (!context->nvrhiDevice)
        throw std::runtime_error("nvrhi::vulkan::createDevice failed");
    return context;
}

std::vector<uint8_t> readFile(const fs::path& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("Cannot open " + path.string());
    const auto size = file.tellg();
    std::vector<uint8_t> result(static_cast<size_t>(size));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(result.data()), size);
    return result;
}

uint64_t hashBytes(const void* data, size_t size)
{
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

uint32_t hash32(uint32_t value)
{
    const uint32_t state = value * 747796405u + 2891336453u;
    const uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float unitFloat(uint32_t value) { return float(value >> 8u) * (1.f / 16777216.f); }

template <typename T>
nvrhi::BufferHandle createBuffer(nvrhi::IDevice* device, size_t count, const char* name,
    bool uav = false, bool accelInput = false, nvrhi::CpuAccessMode cpuAccess = nvrhi::CpuAccessMode::None)
{
    nvrhi::BufferDesc desc;
    desc.byteSize = sizeof(T) * count;
    desc.structStride = sizeof(T);
    desc.debugName = name;
    desc.canHaveUAVs = uav;
    desc.isAccelStructBuildInput = accelInput;
    desc.cpuAccess = cpuAccess;
    desc.initialState = nvrhi::ResourceStates::Common;
    desc.keepInitialState = true;
    auto result = device->createBuffer(desc);
    if (!result) throw std::runtime_error(std::string("createBuffer failed: ") + name);
    return result;
}

nvrhi::BindingLayoutHandle createLayout(nvrhi::IDevice* device, bool rayQuery)
{
    nvrhi::VulkanBindingOffsets offsets;
    offsets.shaderResource = offsets.sampler = offsets.constantBuffer = offsets.unorderedAccess = 0;
    nvrhi::BindingLayoutDesc desc;
    desc.visibility = nvrhi::ShaderType::Compute;
    desc.registerSpace = 0;
    desc.registerSpaceIsDescriptorSet = true;
    desc.bindingOffsets = offsets;
    if (rayQuery) {
        desc.addItem(nvrhi::BindingLayoutItem::RayTracingAccelStruct(0));
        desc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1));
        desc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(2));
    } else {
        desc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0));
        desc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(1));
    }
    auto layout = device->createBindingLayout(desc);
    if (!layout) throw std::runtime_error("createBindingLayout failed");
    return layout;
}

struct PipelineInfo
{
    nvrhi::ShaderHandle shader;
    nvrhi::ComputePipelineHandle pipeline;
    double coldMilliseconds = 0;
    double warmMilliseconds = 0;
};

PipelineInfo createPipeline(nvrhi::IDevice* device, const fs::path& path, nvrhi::IBindingLayout* layout)
{
    const auto binary = readFile(path);
    nvrhi::ShaderDesc shaderDesc(nvrhi::ShaderType::Compute);
    shaderDesc.debugName = path.filename().string();
    shaderDesc.entryName = "main";
    PipelineInfo result;
    result.shader = device->createShader(shaderDesc, binary.data(), binary.size());
    if (!result.shader) throw std::runtime_error("createShader failed: " + path.string());
    nvrhi::ComputePipelineDesc pipelineDesc;
    pipelineDesc.setComputeShader(result.shader).addBindingLayout(layout);
    auto start = std::chrono::steady_clock::now();
    result.pipeline = device->createComputePipeline(pipelineDesc);
    auto end = std::chrono::steady_clock::now();
    result.coldMilliseconds = std::chrono::duration<double, std::milli>(end - start).count();
    if (!result.pipeline) throw std::runtime_error("first createComputePipeline failed: " + path.string());
    start = std::chrono::steady_clock::now();
    auto warmPipeline = device->createComputePipeline(pipelineDesc);
    end = std::chrono::steady_clock::now();
    result.warmMilliseconds = std::chrono::duration<double, std::milli>(end - start).count();
    if (!warmPipeline) throw std::runtime_error("second createComputePipeline failed: " + path.string());
    return result;
}

struct RunResult
{
    std::string kernel;
    std::string compiler;
    double coldPipelineMs = 0;
    double warmPipelineMs = 0;
    double gpuMilliseconds = 0;
    uint64_t outputHash = 0;
    std::vector<uint8_t> output;
};

RunResult runKernel(nvrhi::IDevice* device, const fs::path& artifactDir,
    const std::string& kernel, const std::string& compiler,
    nvrhi::IBindingLayout* layout, nvrhi::IBindingSet* bindings,
    nvrhi::IBuffer* output, nvrhi::IBuffer* readback, size_t outputBytes)
{
    auto pipeline = createPipeline(device, artifactDir / (kernel + "." + compiler + ".spv"), layout);
    auto query = device->createTimerQuery();
    auto commandList = device->createCommandList();
    commandList->open();
    commandList->setComputeState(nvrhi::ComputeState().setPipeline(pipeline.pipeline).addBindingSet(bindings));
    for (uint32_t iteration = 0; iteration < kWarmupDispatchCount; ++iteration)
        commandList->dispatch(kElementCount / kThreads);
    commandList->beginTimerQuery(query);
    for (uint32_t iteration = 0; iteration < kDispatchCount; ++iteration)
        commandList->dispatch(kElementCount / kThreads);
    commandList->endTimerQuery(query);
    commandList->copyBuffer(readback, 0, output, 0, outputBytes);
    commandList->close();
    device->executeCommandList(commandList);
    if (!device->waitForIdle()) throw std::runtime_error("waitForIdle failed");
    const float seconds = device->getTimerQueryTime(query);
    void* mapped = device->mapBuffer(readback, nvrhi::CpuAccessMode::Read);
    if (!mapped) throw std::runtime_error("mapBuffer(readback) failed");
    RunResult result;
    result.kernel = kernel;
    result.compiler = compiler;
    result.coldPipelineMs = pipeline.coldMilliseconds;
    result.warmPipelineMs = pipeline.warmMilliseconds;
    result.gpuMilliseconds = double(seconds) * 1000.0 / kDispatchCount;
    result.output.resize(outputBytes);
    std::memcpy(result.output.data(), mapped, outputBytes);
    device->unmapBuffer(readback);
    result.outputHash = hashBytes(result.output.data(), result.output.size());
    return result;
}

nvrhi::rt::AccelStructHandle buildScene(nvrhi::IDevice* device)
{
    struct Vertex { float x, y, z; };
    const std::array vertices = { Vertex{-1.f, -1.f, 0.f}, Vertex{1.f, -1.f, 0.f}, Vertex{0.f, 1.f, 0.f} };
    auto vertexBuffer = createBuffer<Vertex>(device, vertices.size(), "UPT00 triangle vertices", false, true);
    nvrhi::rt::GeometryTriangles triangles;
    triangles.vertexBuffer = vertexBuffer;
    triangles.vertexFormat = nvrhi::Format::RGB32_FLOAT;
    triangles.vertexCount = uint32_t(vertices.size());
    triangles.vertexStride = sizeof(Vertex);
    nvrhi::rt::GeometryDesc geometry;
    geometry.setTriangles(triangles).setFlags(nvrhi::rt::GeometryFlags::Opaque);
    nvrhi::rt::AccelStructDesc blasDesc;
    blasDesc.addBottomLevelGeometry(geometry).setBuildFlags(nvrhi::rt::AccelStructBuildFlags::PreferFastTrace).setDebugName("UPT00 BLAS");
    auto blas = device->createAccelStruct(blasDesc);
    nvrhi::rt::AccelStructDesc tlasDesc;
    tlasDesc.setTopLevelMaxInstances(1).setBuildFlags(nvrhi::rt::AccelStructBuildFlags::PreferFastTrace).setDebugName("UPT00 TLAS");
    auto tlas = device->createAccelStruct(tlasDesc);
    if (!blas || !tlas) throw std::runtime_error("createAccelStruct failed");
    nvrhi::rt::InstanceDesc instance;
    instance.setBLAS(blas).setInstanceMask(0xff).setInstanceID(7).setFlags(nvrhi::rt::InstanceFlags::TriangleCullDisable);
    auto commandList = device->createCommandList();
    commandList->open();
    commandList->writeBuffer(vertexBuffer, vertices.data(), sizeof(vertices));
    commandList->buildBottomLevelAccelStruct(blas, &geometry, 1, blasDesc.buildFlags);
    commandList->buildTopLevelAccelStruct(tlas, &instance, 1, tlasDesc.buildFlags);
    commandList->close();
    device->executeCommandList(commandList);
    if (!device->waitForIdle()) throw std::runtime_error("AS build waitForIdle failed");
    return tlas;
}

template <typename T>
void upload(nvrhi::IDevice* device, nvrhi::IBuffer* buffer, const std::vector<T>& values)
{
    auto commandList = device->createCommandList();
    commandList->open();
    commandList->writeBuffer(buffer, values.data(), values.size() * sizeof(T));
    commandList->close();
    device->executeCommandList(commandList);
    if (!device->waitForIdle()) throw std::runtime_error("upload waitForIdle failed");
}

void compare(const RunResult& slang, const RunResult& dxc)
{
    if (slang.output != dxc.output)
        throw std::runtime_error(slang.kernel + " Slang/DXC readbacks differ");
}

template <typename T>
void compareCpuOracle(const RunResult& gpu, const std::vector<T>& expected)
{
    const size_t expectedBytes = expected.size() * sizeof(T);
    if (gpu.output.size() != expectedBytes || std::memcmp(gpu.output.data(), expected.data(), expectedBytes) != 0)
        throw std::runtime_error(gpu.kernel + " GPU readback differs from the CPU oracle");
}

std::string hexHash(uint64_t hash)
{
    std::ostringstream stream;
    stream << "0x" << std::hex << std::setw(16) << std::setfill('0') << hash;
    return stream.str();
}
}

int main(int argc, char** argv)
{
    try
    {
        const fs::path artifactDir = argc > 1 ? fs::path(argv[1]) : fs::current_path();
        const fs::path reportPath = argc > 2 ? fs::path(argv[2]) : artifactDir / "gpu_comparison.csv";
        const bool dxcFirst = argc > 3 && std::string(argv[3]) == "dxc-first";
        const char* orderName = dxcFirst ? "dxc-first" : "slang-first";
        auto context = createContext();
        auto* device = context->nvrhiDevice.Get();
        std::vector<RunResult> results;

        auto regularLayout = createLayout(device, false);
        auto structuredInput = createBuffer<upt00::StructuredMathInput>(device, kElementCount, "UPT00 structured input");
        auto structuredOutput = createBuffer<upt00::StructuredMathOutput>(device, kElementCount, "UPT00 structured output", true);
        auto structuredReadback = createBuffer<upt00::StructuredMathOutput>(device, kElementCount, "UPT00 structured readback", false, false, nvrhi::CpuAccessMode::Read);
        std::vector<upt00::StructuredMathInput> structuredValues(kElementCount);
        for (uint32_t i = 0; i < kElementCount; ++i) {
            structuredValues[i].words = { i, i * 3u + 1u, i ^ 0xa5a5a5a5u, 0xffffffffu - i };
            structuredValues[i].values = { float(i % 251), float(i % 97) * .25f, -float(i % 37), float(i % 13) };
        }
        upload(device, structuredInput, structuredValues);
        auto structuredBindings = device->createBindingSet(
            nvrhi::BindingSetDesc()
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(0, structuredInput))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(1, structuredOutput)), regularLayout);
        RunResult structuredSlang, structuredDxc;
        if (dxcFirst) {
            structuredDxc = runKernel(device, artifactDir, "structured_math", "dxc", regularLayout, structuredBindings, structuredOutput, structuredReadback, kElementCount * sizeof(upt00::StructuredMathOutput));
            structuredSlang = runKernel(device, artifactDir, "structured_math", "slang", regularLayout, structuredBindings, structuredOutput, structuredReadback, kElementCount * sizeof(upt00::StructuredMathOutput));
        } else {
            structuredSlang = runKernel(device, artifactDir, "structured_math", "slang", regularLayout, structuredBindings, structuredOutput, structuredReadback, kElementCount * sizeof(upt00::StructuredMathOutput));
            structuredDxc = runKernel(device, artifactDir, "structured_math", "dxc", regularLayout, structuredBindings, structuredOutput, structuredReadback, kElementCount * sizeof(upt00::StructuredMathOutput));
        }
        compare(structuredSlang, structuredDxc);
        std::vector<upt00::StructuredMathOutput> structuredExpected(kElementCount);
        for (uint32_t i = 0; i < kElementCount; ++i) {
            const auto& input = structuredValues[i];
            auto& output = structuredExpected[i];
            output.words = { hash32(input.words.x ^ i), hash32(input.words.y + input.words.x),
                input.words.z ^ 0x9e3779b9u, input.words.w + i * 17u };
            output.values = { input.values.x * 2.f + 1.f, input.values.y * 2.f - 2.f,
                input.values.z * 2.f + .5f, input.values.w * 2.f + 4.f };
        }
        compareCpuOracle(structuredSlang, structuredExpected);
        results.push_back(std::move(structuredSlang)); results.push_back(std::move(structuredDxc));

        auto candidateBuffer = createBuffer<upt00::Candidate>(device, size_t(kElementCount) * 4, "UPT00 candidates");
        auto reservoirOutput = createBuffer<upt00::Reservoir>(device, kElementCount, "UPT00 reservoir output", true);
        auto reservoirReadback = createBuffer<upt00::Reservoir>(device, kElementCount, "UPT00 reservoir readback", false, false, nvrhi::CpuAccessMode::Read);
        std::vector<upt00::Candidate> candidates(size_t(kElementCount) * 4);
        for (uint32_t i = 0; i < kElementCount; ++i) for (uint32_t j = 0; j < 4; ++j) {
            auto& c = candidates[size_t(i) * 4 + j];
            c.weight = (j == 1 && (i & 7u) == 0) ? -1.f : .25f + float((i + j) % 19);
            c.target = .5f + float((i * 3 + j) % 23);
            c.sampleId = i * 4 + j;
            c.status = (j == 3 && (i & 3u) == 0) ? 1u : 2u;
            c.radiance = { float(j + 1), float(i % 31) * .1f, float((i + j) % 17) * .2f };
            c.padding = 0;
        }
        upload(device, candidateBuffer, candidates);
        auto reservoirBindings = device->createBindingSet(
            nvrhi::BindingSetDesc()
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(0, candidateBuffer))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(1, reservoirOutput)), regularLayout);
        RunResult reservoirSlang, reservoirDxc;
        if (dxcFirst) {
            reservoirDxc = runKernel(device, artifactDir, "reservoir_update", "dxc", regularLayout, reservoirBindings, reservoirOutput, reservoirReadback, kElementCount * sizeof(upt00::Reservoir));
            reservoirSlang = runKernel(device, artifactDir, "reservoir_update", "slang", regularLayout, reservoirBindings, reservoirOutput, reservoirReadback, kElementCount * sizeof(upt00::Reservoir));
        } else {
            reservoirSlang = runKernel(device, artifactDir, "reservoir_update", "slang", regularLayout, reservoirBindings, reservoirOutput, reservoirReadback, kElementCount * sizeof(upt00::Reservoir));
            reservoirDxc = runKernel(device, artifactDir, "reservoir_update", "dxc", regularLayout, reservoirBindings, reservoirOutput, reservoirReadback, kElementCount * sizeof(upt00::Reservoir));
        }
        compare(reservoirSlang, reservoirDxc);
        std::vector<upt00::Reservoir> reservoirExpected(kElementCount);
        for (uint32_t i = 0; i < kElementCount; ++i) {
            auto& reservoir = reservoirExpected[i];
            for (uint32_t j = 0; j < 4; ++j) {
                const auto& candidate = candidates[size_t(i) * 4 + j];
                const bool validPositive = candidate.status == 2u && std::isfinite(candidate.weight) &&
                    std::isfinite(candidate.target) && candidate.weight > 0.f && candidate.target > 0.f;
                if (!validPositive) continue;
                const float newWeightSum = reservoir.weightSum + candidate.weight;
                const float selection = unitFloat(hash32(candidate.sampleId ^ (i * 0x9e3779b9u) ^ j));
                if (selection * newWeightSum < candidate.weight) {
                    reservoir.selectedTarget = candidate.target;
                    reservoir.selectedId = candidate.sampleId;
                    reservoir.selectedRadiance = candidate.radiance;
                    reservoir.flags = 1u;
                }
                reservoir.weightSum = newWeightSum;
                reservoir.effectiveM += 1u;
            }
        }
        compareCpuOracle(reservoirSlang, reservoirExpected);
        results.push_back(std::move(reservoirSlang)); results.push_back(std::move(reservoirDxc));

        auto tlas = buildScene(device);
        auto rayLayout = createLayout(device, true);
        auto rayInput = createBuffer<upt00::RayInput>(device, kElementCount, "UPT00 ray input");
        auto rayOutput = createBuffer<upt00::RayOutput>(device, kElementCount, "UPT00 ray output", true);
        auto rayReadback = createBuffer<upt00::RayOutput>(device, kElementCount, "UPT00 ray readback", false, false, nvrhi::CpuAccessMode::Read);
        std::vector<upt00::RayInput> rays(kElementCount);
        for (uint32_t i = 0; i < kElementCount; ++i) {
            const bool hit = (i & 1u) == 0;
            rays[i] = { { hit ? 0.f : 2.f, 0.f, -2.f }, .001f, { 0.f, 0.f, 1.f }, 10.f };
        }
        upload(device, rayInput, rays);
        auto rayBindings = device->createBindingSet(
            nvrhi::BindingSetDesc()
                .addItem(nvrhi::BindingSetItem::RayTracingAccelStruct(0, tlas))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(1, rayInput))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(2, rayOutput)), rayLayout);
        RunResult raySlang, rayDxc;
        if (dxcFirst) {
            rayDxc = runKernel(device, artifactDir, "ray_query", "dxc", rayLayout, rayBindings, rayOutput, rayReadback, kElementCount * sizeof(upt00::RayOutput));
            raySlang = runKernel(device, artifactDir, "ray_query", "slang", rayLayout, rayBindings, rayOutput, rayReadback, kElementCount * sizeof(upt00::RayOutput));
        } else {
            raySlang = runKernel(device, artifactDir, "ray_query", "slang", rayLayout, rayBindings, rayOutput, rayReadback, kElementCount * sizeof(upt00::RayOutput));
            rayDxc = runKernel(device, artifactDir, "ray_query", "dxc", rayLayout, rayBindings, rayOutput, rayReadback, kElementCount * sizeof(upt00::RayOutput));
        }
        compare(raySlang, rayDxc);
        const auto* rayValues = reinterpret_cast<const upt00::RayOutput*>(raySlang.output.data());
        for (uint32_t i = 0; i < kElementCount; ++i) {
            const bool expectedHit = (i & 1u) == 0;
            if (rayValues[i].hit != (expectedHit ? 1u : 0u) ||
                (expectedHit && (rayValues[i].instanceId != 7u || rayValues[i].primitiveId != 0u ||
                    rayValues[i].geometryId != 0u || std::abs(rayValues[i].hitT - 2.f) > 1e-5f)))
                throw std::runtime_error("ray query output did not match the known hit/miss pattern at index " + std::to_string(i));
        }
        results.push_back(std::move(raySlang)); results.push_back(std::move(rayDxc));

        std::ofstream report(reportPath);
        if (!report) throw std::runtime_error("Cannot write " + reportPath.string());
        report << "device,order,kernel,compiler,elements,warmup_dispatches,timed_dispatches,first_pipeline_ms,warm_pipeline_ms,gpu_ms_per_dispatch,output_fnv1a64,readback_match\n";
        for (const auto& result : results) {
            report << '"' << context->properties.deviceName << "\"," << orderName << ',' << result.kernel << ',' << result.compiler << ','
                << kElementCount << ',' << kWarmupDispatchCount << ',' << kDispatchCount << ',' << std::fixed << std::setprecision(6)
                << result.coldPipelineMs << ',' << result.warmPipelineMs << ',' << result.gpuMilliseconds << ','
                << hexHash(result.outputHash) << ",yes\n";
        }
        report.close();
        std::cout << "UPT-00 ABI/readback comparison passed on " << context->properties.deviceName << '\n'
                  << "order=" << orderName << " elements=" << kElementCount << " warmup_dispatches=" << kWarmupDispatchCount
                  << " timed_dispatches=" << kDispatchCount << '\n'
                  << "report=" << reportPath.string() << '\n';
        if (context->messages.sawError) throw std::runtime_error("NVRHI reported an error");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "UPT-00 FAILED: " << error.what() << '\n';
        return 1;
    }
}
