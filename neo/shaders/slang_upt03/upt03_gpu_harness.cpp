#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <vulkan/vulkan.hpp>
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

#include <nvrhi/nvrhi.h>
#include <nvrhi/vulkan.h>

#include "upt02_unified_reservoir_codec.h"

#include <algorithm>
#include <array>
#include <cstdint>
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

namespace {

constexpr uint32_t kOutputCount = 8;
constexpr uint32_t kCandidatesPerOutput = 4;
constexpr uint32_t kExpectedGeneration = 0x10203040u;
constexpr uint64_t kExpectedOutputHash = 0x72ee283cfb295d70ull;

struct GpuCandidate {
	uint32_t status = 0;
	uint32_t eventKind = 0;
	uint32_t lobeMask = 0;
	uint32_t flags = 0;
	uint32_t age = 0;
	uint32_t pathLength = 0;
	uint32_t reconnectionVertexLength = 0;
	uint32_t identity0 = 0;
	uint32_t identity1 = 0;
	uint32_t replayKey = 0;
	uint32_t replayIndex = 0;
	uint32_t generationFingerprint = 0;
	float sampleCoord0 = 0.0f;
	float sampleCoord1 = 0.0f;
	float contributionR = 0.0f;
	float contributionG = 0.0f;
	float contributionB = 0.0f;
	float target = 0.0f;
	float proposalPdf = 0.0f;
	float pathPdf = 0.0f;
	float partialJacobian = 0.0f;
	float accumulatedRouletteProbability = 0.0f;
	float selectionRandom = 0.0f;
	uint32_t padding = 0;
};

static_assert(sizeof(GpuCandidate) == 96, "UPT-03 candidate ABI drifted");
static_assert(sizeof(rb::upt02::PackedReservoir) == 64, "UPT-02 packed ABI drifted");

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
		if (severity == nvrhi::MessageSeverity::Error) {
			sawError = true;
		}
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
		if (device) {
			vkDestroyDevice(device, nullptr);
		}
		if (instance) {
			vkDestroyInstance(instance, nullptr);
		}
	}
};

std::unique_ptr<VulkanContext> CreateContext() {
	auto context = std::make_unique<VulkanContext>();
	VULKAN_HPP_DEFAULT_DISPATCHER.init(vkGetInstanceProcAddr);

	VkApplicationInfo applicationInfo = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
	applicationInfo.pApplicationName = "rbdoom UPT-03 deterministic codec";
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
		VkPhysicalDeviceVulkan12Features vulkan12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
		VkPhysicalDeviceFeatures2 features = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
		features.pNext = &vulkan12;
		vkGetPhysicalDeviceFeatures2(candidate, &features);
		if (!vulkan12.scalarBlockLayout) {
			continue;
		}
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
		if (context->physicalDevice) {
			break;
		}
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
	VkPhysicalDeviceVulkan12Features vulkan12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
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
	if (!context->nvrhiDevice) {
		throw std::runtime_error("nvrhi::vulkan::createDevice failed");
	}
	return context;
}

std::vector<uint8_t> ReadFile(const fs::path& path) {
	std::ifstream file(path, std::ios::binary | std::ios::ate);
	if (!file) {
		throw std::runtime_error("Cannot open " + path.string());
	}
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

std::string HexHash(uint64_t hash) {
	std::ostringstream stream;
	stream << "0x" << std::hex << std::setw(16) << std::setfill('0') << hash;
	return stream.str();
}

template <typename T>
nvrhi::BufferHandle CreateBuffer(
	nvrhi::IDevice* device,
	size_t count,
	const char* name,
	bool uav = false,
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
	if (!buffer) {
		throw std::runtime_error(std::string("createBuffer failed: ") + name);
	}
	return buffer;
}

GpuCandidate MakePositive(uint32_t eventKind, uint32_t identity, float selectionRandom) {
	GpuCandidate candidate = {};
	candidate.status = 2;
	candidate.eventKind = eventKind;
	candidate.lobeMask = rb::upt02::LobeDiffuse;
	candidate.flags = rb::upt02::FlagVisibilityKnown | rb::upt02::FlagVisibilityPassed;
	candidate.identity0 = identity;
	candidate.identity1 = 3;
	candidate.replayKey = eventKind == 4 ? 0x12345678u : 0u;
	candidate.replayIndex = eventKind == 4 ? 9u : 0u;
	candidate.generationFingerprint = kExpectedGeneration;
	candidate.sampleCoord0 = eventKind == 3 ? -0.375f : 0.25f;
	candidate.sampleCoord1 = eventKind == 3 ? 0.625f : 0.5f;
	candidate.contributionR = 1.25f;
	candidate.contributionG = 0.5f;
	candidate.contributionB = 0.125f;
	candidate.target = 2.0f;
	candidate.proposalPdf = 0.25f;
	candidate.pathPdf = 0.125f;
	candidate.partialJacobian = 0.75f;
	candidate.accumulatedRouletteProbability = 1.0f;
	candidate.selectionRandom = selectionRandom;
	return candidate;
}

GpuCandidate MakeZero(uint32_t identity) {
	GpuCandidate candidate = MakePositive(1, identity, 0.0f);
	candidate.status = 1;
	candidate.contributionR = candidate.contributionG = candidate.contributionB = 0.0f;
	candidate.target = 0.0f;
	return candidate;
}

std::vector<GpuCandidate> MakeCorpus() {
	std::vector<GpuCandidate> values(kOutputCount * kCandidatesPerOutput);
	auto set = [&](uint32_t output, uint32_t slot, GpuCandidate candidate) {
		values[output * kCandidatesPerOutput + slot] = candidate;
	};

	set(0, 0, MakeZero(10));
	set(0, 1, MakePositive(1, 11, 0.75f));
	set(0, 2, MakeZero(12));
	set(0, 3, GpuCandidate{});
	for (uint32_t slot = 0; slot < 4; ++slot) {
		set(1, slot, MakeZero(20 + slot));
	}
	GpuCandidate zeroPdf = MakePositive(1, 30, 0.0f);
	zeroPdf.proposalPdf = 0.0f;
	set(2, 0, zeroPdf);
	GpuCandidate nanContribution = MakePositive(1, 31, 0.0f);
	nanContribution.contributionG = std::numeric_limits<float>::quiet_NaN();
	set(2, 1, nanContribution);
	set(3, 0, MakePositive(1, 40, 0.0f));
	GpuCandidate winner = MakePositive(2, 41, 0.1f);
	winner.target = 4.0f;
	winner.proposalPdf = 0.25f;
	set(3, 1, winner);
	GpuCandidate kept = MakePositive(1, 42, 0.99f);
	kept.target = 1.0f;
	kept.proposalPdf = 1.0f;
	set(3, 2, kept);
	GpuCandidate indirect = MakePositive(4, 50, 0.0f);
	indirect.flags |= rb::upt02::FlagGlobal | rb::upt02::FlagReplayable;
	indirect.pathLength = 3;
	indirect.reconnectionVertexLength = 2;
	indirect.pathPdf = 0.0125f;
	indirect.accumulatedRouletteProbability = 0.125f;
	set(4, 0, indirect);
	set(5, 0, MakePositive(3, 60, 0.0f));
	GpuCandidate stale = MakePositive(1, 70, 0.0f);
	stale.generationFingerprint += 1;
	set(6, 0, stale);
	set(7, 0, MakeZero(80));
	set(7, 1, MakePositive(2, 81, 0.0f));
	return values;
}

rb::upt02::Candidate ToCpuCandidate(const GpuCandidate& input) {
	rb::upt02::Candidate candidate = {};
	candidate.status = static_cast<rb::upt02::CandidateStatus>(input.status);
	candidate.eventKind = static_cast<rb::upt02::EventKind>(input.eventKind);
	candidate.lobeMask = static_cast<uint8_t>(input.lobeMask);
	candidate.flags = static_cast<uint8_t>(input.flags);
	candidate.age = static_cast<uint8_t>(input.age);
	candidate.pathLength = static_cast<uint8_t>(input.pathLength);
	candidate.reconnectionVertexLength = static_cast<uint8_t>(input.reconnectionVertexLength);
	candidate.identity0 = input.identity0;
	candidate.identity1 = input.identity1;
	candidate.replayKey = input.replayKey;
	candidate.replayIndex = input.replayIndex;
	candidate.generationFingerprint = input.generationFingerprint;
	candidate.sampleCoord0 = input.sampleCoord0;
	candidate.sampleCoord1 = input.sampleCoord1;
	candidate.contribution = { input.contributionR, input.contributionG, input.contributionB };
	candidate.target = input.target;
	candidate.proposalPdf = input.proposalPdf;
	candidate.pathPdf = input.pathPdf;
	candidate.partialJacobian = input.partialJacobian;
	candidate.accumulatedRouletteProbability = input.accumulatedRouletteProbability;
	return candidate;
}

std::array<rb::upt02::PackedReservoir, kOutputCount> MakeCpuExpected(
	const std::vector<GpuCandidate>& inputs) {
	std::array<rb::upt02::PackedReservoir, kOutputCount> expected = {};
	for (uint32_t output = 0; output < kOutputCount; ++output) {
		rb::upt02::LogicalReservoir reservoir = {};
		for (uint32_t slot = 0; slot < kCandidatesPerOutput; ++slot) {
			const GpuCandidate& input = inputs[output * kCandidatesPerOutput + slot];
			rb::upt02::StreamCandidate(
				reservoir, ToCpuCandidate(input), input.selectionRandom, kExpectedGeneration);
		}
		if (!rb::upt02::PackReservoir(reservoir, kExpectedGeneration, expected[output])) {
			throw std::runtime_error("CPU oracle failed to pack output " + std::to_string(output));
		}
	}
	return expected;
}

std::vector<uint8_t> DispatchAndReadback(
	nvrhi::IDevice* device,
	nvrhi::IComputePipeline* pipeline,
	nvrhi::IBindingSet* bindings,
	nvrhi::IBuffer* output,
	nvrhi::IBuffer* readback) {
	auto commandList = device->createCommandList();
	commandList->open();
	commandList->setComputeState(nvrhi::ComputeState().setPipeline(pipeline).addBindingSet(bindings));
	commandList->dispatch(1);
	commandList->copyBuffer(readback, 0, output, 0,
		kOutputCount * sizeof(rb::upt02::PackedReservoir));
	commandList->close();
	device->executeCommandList(commandList);
	if (!device->waitForIdle()) {
		throw std::runtime_error("dispatch waitForIdle failed");
	}
	void* mapped = device->mapBuffer(readback, nvrhi::CpuAccessMode::Read);
	if (!mapped) {
		throw std::runtime_error("mapBuffer(readback) failed");
	}
	std::vector<uint8_t> result(kOutputCount * sizeof(rb::upt02::PackedReservoir));
	std::memcpy(result.data(), mapped, result.size());
	device->unmapBuffer(readback);
	return result;
}

} // namespace

int main(int argc, char** argv) {
	try {
		const fs::path artifactDirectory = argc > 1 ? fs::path(argv[1]) : fs::current_path();
		auto context = CreateContext();
		auto* device = context->nvrhiDevice.Get();
		const auto inputs = MakeCorpus();
		const auto expected = MakeCpuExpected(inputs);

		auto inputBuffer = CreateBuffer<GpuCandidate>(device, inputs.size(), "UPT03 candidates");
		auto outputBuffer = CreateBuffer<rb::upt02::PackedReservoir>(
			device, kOutputCount, "UPT03 packed output", true);
		auto readbackBuffer = CreateBuffer<rb::upt02::PackedReservoir>(
			device, kOutputCount, "UPT03 readback", false, nvrhi::CpuAccessMode::Read);
		auto uploadList = device->createCommandList();
		uploadList->open();
		uploadList->writeBuffer(inputBuffer, inputs.data(), inputs.size() * sizeof(GpuCandidate));
		uploadList->close();
		device->executeCommandList(uploadList);
		if (!device->waitForIdle()) {
			throw std::runtime_error("upload waitForIdle failed");
		}

		nvrhi::VulkanBindingOffsets offsets;
		offsets.shaderResource = offsets.sampler = offsets.constantBuffer = offsets.unorderedAccess = 0;
		nvrhi::BindingLayoutDesc layoutDesc;
		layoutDesc.visibility = nvrhi::ShaderType::Compute;
		layoutDesc.registerSpace = 0;
		layoutDesc.registerSpaceIsDescriptorSet = true;
		layoutDesc.bindingOffsets = offsets;
		layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0));
		layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(1));
		auto layout = device->createBindingLayout(layoutDesc);
		auto bindings = device->createBindingSet(
			nvrhi::BindingSetDesc()
				.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(0, inputBuffer))
				.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(1, outputBuffer)),
			layout);
		if (!layout || !bindings) {
			throw std::runtime_error("UPT-03 binding creation failed");
		}

		const auto spirv = ReadFile(artifactDirectory / "upt03_codec_test.slang.spv");
		nvrhi::ShaderDesc shaderDesc(nvrhi::ShaderType::Compute);
		shaderDesc.debugName = "UPT03 deterministic codec";
		shaderDesc.entryName = "main";
		auto shader = device->createShader(shaderDesc, spirv.data(), spirv.size());
		nvrhi::ComputePipelineDesc pipelineDesc;
		pipelineDesc.setComputeShader(shader).addBindingLayout(layout);
		auto pipeline = device->createComputePipeline(pipelineDesc);
		if (!shader || !pipeline) {
			throw std::runtime_error("UPT-03 shader/pipeline creation failed");
		}

		const auto first = DispatchAndReadback(
			device, pipeline, bindings, outputBuffer, readbackBuffer);
		const auto second = DispatchAndReadback(
			device, pipeline, bindings, outputBuffer, readbackBuffer);
		const size_t expectedBytes = sizeof(expected);
		if (first.size() != expectedBytes ||
			std::memcmp(first.data(), expected.data(), expectedBytes) != 0) {
			throw std::runtime_error("GPU reservoir bytes differ from UPT-02 CPU oracle");
		}
		if (first != second) {
			throw std::runtime_error("fixed-input GPU output changed between dispatches");
		}
		const uint64_t hash = HashBytes(first.data(), first.size());
		if (hash != kExpectedOutputHash) {
			throw std::runtime_error("fixed corpus hash changed: " + HexHash(hash));
		}
		std::cout << "UPT-03 CPU/GPU byte comparison passed on " << context->properties.deviceName << '\n'
			<< "candidate_stride=" << sizeof(GpuCandidate)
			<< " reservoir_stride=" << sizeof(rb::upt02::PackedReservoir)
			<< " candidates=" << inputs.size() << " outputs=" << kOutputCount
			<< " dispatches_per_trial=1 validation_trials=2 rays=0 hash=" << HexHash(hash) << '\n';
		if (context->messages.sawError) {
			throw std::runtime_error("NVRHI reported an error");
		}
		return 0;
	} catch (const std::exception& error) {
		std::cerr << "UPT-03 FAILED: " << error.what() << '\n';
		return 1;
	}
}
