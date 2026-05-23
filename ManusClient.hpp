#ifndef _MANUS_CLIENT_HPP_
#define _MANUS_CLIENT_HPP_

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <zmq.hpp>

#include "ClientPlatformSpecific.hpp"
#include "ManusSDK.h"

enum class ClientReturnCode : int
{
	ClientReturnCode_Success = 0,
	ClientReturnCode_FailedPlatformSpecificInitialization,
	ClientReturnCode_FailedToInitialize,
	ClientReturnCode_FailedToFindHosts,
	ClientReturnCode_FailedToConnect,
	ClientReturnCode_FailedToShutDownSDK,
	ClientReturnCode_FailedPlatformSpecificShutdown,
};

class ClientSkeleton
{
public:
	RawSkeletonInfo info;
	std::vector<SkeletonNode> nodes;
};

class ClientSkeletonCollection
{
public:
	std::vector<ClientSkeleton> skeletons;
};

class ManusClient : public SDKClientPlatformSpecific
{
public:
	ManusClient();
	~ManusClient();

	ClientReturnCode Initialize();
	ClientReturnCode Run();
	ClientReturnCode ShutDown();

	static void OnRawSkeletonStreamCallback(const SkeletonStreamInfo* p_Skeleton);
	static void OnLandscapeCallback(const Landscape* p_Landscape);

	void LoadGloveCalibration(uint32_t p_GloveId, const std::string& p_CalibrationFileName);

	std::string m_ExeDirectory;

protected:
	ClientReturnCode InitializeSDK();
	ClientReturnCode RegisterAllCallbacks();
	ClientReturnCode Connect();

	static ManusClient* s_Instance;

	uint32_t m_FrameId = 0;
	Landscape* m_Landscape = nullptr;
	uint32_t m_FirstLeftGloveID = UINT32_MAX;
	uint32_t m_FirstRightGloveID = UINT32_MAX;

	std::shared_ptr<zmq::context_t> m_ZmqPubContext;
	std::shared_ptr<zmq::socket_t> m_ZmqPublisher;
	std::string m_ZmqHost = "tcp://127.0.0.1:2044";

	bool m_Running = true;
	bool m_LeftGloveCalibrationLoaded = false;
	bool m_RightGloveCalibrationLoaded = false;
};

#endif
