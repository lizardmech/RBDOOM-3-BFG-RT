#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <vulkan/vulkan.hpp>
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

#include <nvrhi/nvrhi.h>
#include <nvrhi/vulkan.h>

#include "upt10_contract_oracle.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

void CheckVk(VkResult result, const char* operation) {
	if (result != VK_SUCCESS) {
		throw std::runtime_error(std::string(operation) + " failed with VkResult " +
			std::to_string(result));
	}
}

class MessageCallback final : public nvrhi::IMessageCallback {
public:
	void message(nvrhi::MessageSeverity severity, const char* messageText) override {
		std::cerr << "NVRHI[" << int(severity) << "]: " << messageText << '\n';
		if (severity == nvrhi::MessageSeverity::Error) sawError = true;
	}
	bool sawError = false;
};

struct VulkanContext {
	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	uint32_t queueFamily = 0;
	VkPhysicalDeviceProperties properties = {};
	MessageCallback messages;
	nvrhi::vulkan::DeviceHandle nvrhiDevice;

	~VulkanContext() {
		nvrhiDevice = nullptr;
		if (device) vkDestroyDevice(device, nullptr);
		if (instance) vkDestroyInstance(instance, nullptr);
	}
};

std::unique_ptr<VulkanContext> CreateContext() {
	auto context = std::make_unique<VulkanContext>();
	VULKAN_HPP_DEFAULT_DISPATCHER.init(vkGetInstanceProcAddr);
	VkApplicationInfo applicationInfo = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
	applicationInfo.pApplicationName = "rbdoom UPT-30 reuse contract";
	applicationInfo.apiVersion = VK_API_VERSION_1_2;
	VkInstanceCreateInfo instanceInfo = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
	instanceInfo.pApplicationInfo = &applicationInfo;
	CheckVk(vkCreateInstance(&instanceInfo, nullptr, &context->instance), "vkCreateInstance");
	VULKAN_HPP_DEFAULT_DISPATCHER.init(context->instance, vkGetInstanceProcAddr);

	uint32_t deviceCount = 0;
	CheckVk(vkEnumeratePhysicalDevices(context->instance, &deviceCount, nullptr),
		"vkEnumeratePhysicalDevices(count)");
	std::vector<VkPhysicalDevice> devices(deviceCount);
	CheckVk(vkEnumeratePhysicalDevices(context->instance, &deviceCount, devices.data()),
		"vkEnumeratePhysicalDevices");
	for (VkPhysicalDevice candidate : devices) {
		VkPhysicalDeviceVulkan12Features vulkan12 = {
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
		VkPhysicalDeviceFeatures2 features = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
		features.pNext = &vulkan12;
		vkGetPhysicalDeviceFeatures2(candidate, &features);
		if (!vulkan12.scalarBlockLayout) continue;
		uint32_t queueCount = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queueCount, nullptr);
		std::vector<VkQueueFamilyProperties> queues(queueCount);
		vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queueCount, queues.data());
		for (uint32_t queue = 0; queue < queueCount; ++queue) {
			if ((queues[queue].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
				(VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
				context->physicalDevice = candidate;
				context->queueFamily = queue;
				break;
			}
		}
		if (context->physicalDevice) break;
	}
	if (!context->physicalDevice) {
		throw std::runtime_error("No Vulkan 1.2 compute device supports scalarBlockLayout");
	}

	vkGetPhysicalDeviceProperties(context->physicalDevice, &context->properties);
	float priority = 1.0f;
	VkDeviceQueueCreateInfo queueInfo = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
	queueInfo.queueFamilyIndex = context->queueFamily;
	queueInfo.queueCount = 1;
	queueInfo.pQueuePriorities = &priority;
	VkPhysicalDeviceVulkan12Features vulkan12 = {
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
	vulkan12.scalarBlockLayout = VK_TRUE;
	VkDeviceCreateInfo deviceInfo = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
	deviceInfo.pNext = &vulkan12;
	deviceInfo.queueCreateInfoCount = 1;
	deviceInfo.pQueueCreateInfos = &queueInfo;
	CheckVk(vkCreateDevice(context->physicalDevice, &deviceInfo, nullptr, &context->device),
		"vkCreateDevice");
	VULKAN_HPP_DEFAULT_DISPATCHER.init(
		context->instance, vkGetInstanceProcAddr, context->device);
	vkGetDeviceQueue(context->device, context->queueFamily, 0, &context->queue);

	nvrhi::vulkan::DeviceDesc desc = {};
	desc.errorCB = &context->messages;
	desc.instance = context->instance;
	desc.physicalDevice = context->physicalDevice;
	desc.device = context->device;
	desc.graphicsQueue = context->queue;
	desc.graphicsQueueIndex = int(context->queueFamily);
	context->nvrhiDevice = nvrhi::vulkan::createDevice(desc);
	if (!context->nvrhiDevice) throw std::runtime_error("nvrhi::vulkan::createDevice failed");
	return context;
}

std::vector<uint8_t> ReadFile(const fs::path& path) {
	std::ifstream file(path, std::ios::binary | std::ios::ate);
	if (!file) throw std::runtime_error("Cannot open " + path.string());
	const auto size = file.tellg();
	std::vector<uint8_t> bytes(static_cast<size_t>(size));
	file.seekg(0);
	file.read(reinterpret_cast<char*>(bytes.data()), size);
	return bytes;
}

uint64_t HashBytes(const void* data, size_t size) {
	const auto* bytes = static_cast<const uint8_t*>(data);
	uint64_t hash = 1469598103934665603ull;
	for (size_t index = 0; index < size; ++index) {
		hash ^= bytes[index];
		hash *= 1099511628211ull;
	}
	return hash;
}

template <typename T>
nvrhi::BufferHandle CreateBuffer(nvrhi::IDevice* device, size_t count,
	const char* name, bool uav = false,
	nvrhi::CpuAccessMode cpuAccess = nvrhi::CpuAccessMode::None) {
	nvrhi::BufferDesc desc;
	desc.byteSize = sizeof(T) * count;
	desc.structStride = sizeof(T);
	desc.debugName = name;
	desc.canHaveUAVs = uav;
	desc.cpuAccess = cpuAccess;
	desc.initialState = nvrhi::ResourceStates::Common;
	desc.keepInitialState = true;
	auto buffer = device->createBuffer(desc);
	if (!buffer) throw std::runtime_error(std::string("createBuffer failed: ") + name);
	return buffer;
}

std::vector<uint8_t> DispatchAndReadback(nvrhi::IDevice* device,
	nvrhi::IComputePipeline* pipeline, nvrhi::IBindingSet* bindings,
	nvrhi::IBuffer* output, nvrhi::IBuffer* readback) {
	auto commandList = device->createCommandList();
	commandList->open();
	commandList->setComputeState(
		nvrhi::ComputeState().setPipeline(pipeline).addBindingSet(bindings));
	commandList->dispatch(1);
	commandList->copyBuffer(readback, 0, output, 0,
		rb::upt10::PairCount * sizeof(rb::upt10::Result));
	commandList->close();
	device->executeCommandList(commandList);
	if (!device->waitForIdle()) throw std::runtime_error("dispatch waitForIdle failed");
	void* mapped = device->mapBuffer(readback, nvrhi::CpuAccessMode::Read);
	if (!mapped) throw std::runtime_error("mapBuffer(readback) failed");
	std::vector<uint8_t> result(rb::upt10::PairCount * sizeof(rb::upt10::Result));
	std::memcpy(result.data(), mapped, result.size());
	device->unmapBuffer(readback);
	return result;
}

} // namespace

int main(int argc, char** argv) try {
	const fs::path artifactDirectory = argc > 1 ? fs::path(argv[1]) : fs::current_path();
	const rb::upt10::Corpus corpus = rb::upt10::MakeCorpus();
	const auto expected = rb::upt10::MakeExpected(corpus);
	rb::upt10::ValidateExpected(expected);

	auto context = CreateContext();
	auto* device = context->nvrhiDevice.Get();
	auto proposals = CreateBuffer<rb::upt10::Proposal>(device,
		corpus.proposals.size(), "UPT30 proposals");
	auto controls = CreateBuffer<rb::upt10::Control>(device,
		corpus.controls.size(), "UPT30 controls");
	auto output = CreateBuffer<rb::upt10::Result>(device,
		rb::upt10::PairCount, "UPT30 output", true);
	auto readback = CreateBuffer<rb::upt10::Result>(device,
		rb::upt10::PairCount, "UPT30 readback", false, nvrhi::CpuAccessMode::Read);

	auto upload = device->createCommandList();
	upload->open();
	upload->writeBuffer(proposals, corpus.proposals.data(), sizeof(corpus.proposals));
	upload->writeBuffer(controls, corpus.controls.data(), sizeof(corpus.controls));
	upload->close();
	device->executeCommandList(upload);
	if (!device->waitForIdle()) throw std::runtime_error("upload waitForIdle failed");

	nvrhi::VulkanBindingOffsets offsets;
	offsets.shaderResource = offsets.sampler = offsets.constantBuffer = offsets.unorderedAccess = 0;
	nvrhi::BindingLayoutDesc layoutDesc;
	layoutDesc.visibility = nvrhi::ShaderType::Compute;
	layoutDesc.registerSpace = 0;
	layoutDesc.registerSpaceIsDescriptorSet = true;
	layoutDesc.bindingOffsets = offsets;
	layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0));
	layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1));
	layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(2));
	auto layout = device->createBindingLayout(layoutDesc);
	auto bindings = device->createBindingSet(
		nvrhi::BindingSetDesc()
			.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(0, proposals))
			.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(1, controls))
			.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(2, output)), layout);
	if (!layout || !bindings) throw std::runtime_error("UPT-30 binding creation failed");

	const auto spirv = ReadFile(artifactDirectory / "upt10_reuse_contract_probe.slang.spv");
	nvrhi::ShaderDesc shaderDesc(nvrhi::ShaderType::Compute);
	shaderDesc.debugName = "UPT30 shared reuse contract";
	shaderDesc.entryName = "main";
	auto shader = device->createShader(shaderDesc, spirv.data(), spirv.size());
	nvrhi::ComputePipelineDesc pipelineDesc;
	pipelineDesc.setComputeShader(shader).addBindingLayout(layout);
	auto pipeline = device->createComputePipeline(pipelineDesc);
	if (!shader || !pipeline) throw std::runtime_error("UPT-30 shader/pipeline creation failed");

	const auto first = DispatchAndReadback(device, pipeline, bindings, output, readback);
	const auto second = DispatchAndReadback(device, pipeline, bindings, output, readback);
	if (first.size() != sizeof(expected) ||
		std::memcmp(first.data(), expected.data(), sizeof(expected)) != 0) {
		throw std::runtime_error("GPU result bytes differ from UPT-30 CPU oracle");
	}
	if (first != second) throw std::runtime_error("fixed-input GPU output changed");
	const uint64_t hash = HashBytes(first.data(), first.size());
	std::ostringstream hashText;
	hashText << "0x" << std::hex << std::setw(16) << std::setfill('0') << hash;
	std::cout << "UPT-30 CPU/GPU byte comparison passed on "
		<< context->properties.deviceName << " pairs=" << rb::upt10::PairCount
		<< " bytes=" << first.size() << " rays=0 hash=" << hashText.str() << '\n';
	if (context->messages.sawError) throw std::runtime_error("NVRHI reported an error");
	if (argc > 2) {
		std::ofstream stamp(argv[2], std::ios::binary | std::ios::trunc);
		if (!stamp) throw std::runtime_error("failed to create GPU parity stamp");
		stamp << "UPT-30 CPU/GPU bytes passed hash=" << hashText.str() << '\n';
	}
	return 0;
} catch (const std::exception& error) {
	std::cerr << "UPT-30 GPU harness FAILED: " << error.what() << '\n';
	return 1;
}
