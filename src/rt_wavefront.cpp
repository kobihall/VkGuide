#include <rt_wavefront.h>

#include <vk_engine.h>

bool WavefrontPool::ensure(VulkanEngine* engine, CrtResourceTable& resources, uint32_t width, uint32_t height, uint32_t samplesPerFrame)
{
	if (m_allocated && m_width == width && m_height == height && m_samplesPerFrame == samplesPerFrame) {
		return false;
	}

	//the previous render's frames may still be executing against the old pool: a device-wide wait
	//is the obviously-correct way to retire it
	vkDeviceWaitIdle(engine->m_device);
	destroy(engine, resources);

	m_width = width;
	m_height = height;
	m_samplesPerFrame = samplesPerFrame;
	m_size = width * height * samplesPerFrame;

	const VkBufferUsageFlags storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	m_paths = engine->createBuffer(CRT_PATH_STATE_SIZE * m_size, storage, VMA_MEMORY_USAGE_GPU_ONLY);
	m_hits = engine->createBuffer(CRT_HIT_RECORD_SIZE * m_size, storage, VMA_MEMORY_USAGE_GPU_ONLY);
	m_queues = engine->createBuffer(sizeof(uint32_t) * CRT_QUEUE_COUNT * (VkDeviceSize)m_size, storage, VMA_MEMORY_USAGE_GPU_ONLY);
	m_radiance = engine->createBuffer(sizeof(glm::vec4) * (VkDeviceSize)m_size, storage, VMA_MEMORY_USAGE_GPU_ONLY);
	m_shadowRays = engine->createBuffer(CRT_SHADOW_RAY_SIZE * m_size, storage, VMA_MEMORY_USAGE_GPU_ONLY);
	m_sampleBudget = engine->createBuffer(sizeof(uint32_t) * (VkDeviceSize)width * height, storage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
	//the headers are the indirect arguments, reset by the command buffer and read back for the readout
	m_headers = engine->createBuffer(sizeof(CrtQueueHeader) * CRT_QUEUE_COUNT, storage | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
	m_allocated = true;

	resources.setBuffer(CrtBinding::Paths, m_paths.buffer, CRT_PATH_STATE_SIZE * m_size);
	resources.setBuffer(CrtBinding::Hits, m_hits.buffer, CRT_HIT_RECORD_SIZE * m_size);
	resources.setBuffer(CrtBinding::Queues, m_queues.buffer, sizeof(uint32_t) * CRT_QUEUE_COUNT * (VkDeviceSize)m_size);
	resources.setBuffer(CrtBinding::Headers, m_headers.buffer, sizeof(CrtQueueHeader) * CRT_QUEUE_COUNT);
	resources.setBuffer(CrtBinding::Radiance, m_radiance.buffer, sizeof(glm::vec4) * (VkDeviceSize)m_size);
	resources.setBuffer(CrtBinding::SampleBudget, m_sampleBudget.buffer, sizeof(uint32_t) * (VkDeviceSize)width * height);
	resources.setBuffer(CrtBinding::ShadowRays, m_shadowRays.buffer, CRT_SHADOW_RAY_SIZE * m_size);

	const double megabytes = double((CRT_PATH_STATE_SIZE + CRT_HIT_RECORD_SIZE + CRT_SHADOW_RAY_SIZE + 4 * CRT_QUEUE_COUNT + 16) * m_size + 4 * (VkDeviceSize)width * height) / (1024.0 * 1024.0);
	fmt::println("WavefrontPool: {} x {} x {} = {} paths, {:.0f} MB", width, height, samplesPerFrame, m_size, megabytes);
	return true;
}

void WavefrontPool::destroy(VulkanEngine* engine, CrtResourceTable& resources)
{
	if (!m_allocated) {
		return;
	}
	engine->destroyBuffer(m_paths);
	engine->destroyBuffer(m_hits);
	engine->destroyBuffer(m_queues);
	engine->destroyBuffer(m_radiance);
	engine->destroyBuffer(m_sampleBudget);
	engine->destroyBuffer(m_shadowRays);
	engine->destroyBuffer(m_headers);
	m_paths = m_hits = m_queues = m_radiance = m_sampleBudget = m_shadowRays = m_headers = {};
	for (const CrtBinding binding : { CrtBinding::Paths, CrtBinding::Hits, CrtBinding::Queues, CrtBinding::Headers, CrtBinding::Radiance, CrtBinding::SampleBudget, CrtBinding::ShadowRays }) {
		resources.clear(binding);
	}
	m_allocated = false;
}

DispatchSize WavefrontPool::dispatchSize(const KernelVariant& variant, uint32_t bounce) const
{
	switch (variant.domain) {
	case KernelDomain::Pool:
		return { .groups = (m_size + CRT_WORKGROUP - 1) / CRT_WORKGROUP };
	case KernelDomain::Pixels:
		return { .groups = (m_width * m_height + CRT_WORKGROUP - 1) / CRT_WORKGROUP };
	case KernelDomain::CurrentRayQueue:
		return { .indirectBuffer = m_headers.buffer, .indirectOffset = (bounce & 1u) * sizeof(CrtQueueHeader) };
	case KernelDomain::FixedQueue:
		return { .indirectBuffer = m_headers.buffer, .indirectOffset = variant.queue * sizeof(CrtQueueHeader) };
	}
	return {};
}

void WavefrontPool::resetHeader(VkCommandBuffer cmd, uint32_t queue) const
{
	const CrtQueueHeader emptyHeader { 0, 1, 1, 0 };
	vkCmdUpdateBuffer(cmd, m_headers.buffer, queue * sizeof(CrtQueueHeader), sizeof(CrtQueueHeader), &emptyHeader);
}
