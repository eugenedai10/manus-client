#include "ManusClient.hpp"
#include "ManusSDKTypes.h"
#include "ClientLogging.hpp"
#include <iostream>
#include <thread>
#include <zmq.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include "manus_stream.pb.h"
#include "ClientPlatformSpecific.hpp"

using ManusSDK::ClientLog;

ManusClient* ManusClient::s_Instance = nullptr;

ManusClient::ManusClient()
{
	s_Instance = this;
	m_FrameId = 0;
}

ManusClient::~ManusClient()
{
	s_Instance = nullptr;
}

ClientReturnCode ManusClient::Initialize()
{
	if (!PlatformSpecificInitialization())
	{
		return ClientReturnCode::ClientReturnCode_FailedPlatformSpecificInitialization;
	}

	m_ZmqPubContext = std::make_shared<zmq::context_t>(1);
	m_ZmqPublisher = std::make_shared<zmq::socket_t>(*m_ZmqPubContext, ZMQ_PUB);

	// Set send high water mark to prevent memory overflow
	int sndhwm = 10;  // Send high water mark
	m_ZmqPublisher->set(zmq::sockopt::sndhwm, sndhwm);
	int linger = 0;   // Close immediately
	m_ZmqPublisher->set(zmq::sockopt::linger, linger);

	m_ZmqPublisher->bind(m_ZmqHost);

	const ClientReturnCode t_IntializeResult = InitializeSDK();
	if (t_IntializeResult != ClientReturnCode::ClientReturnCode_Success)
	{
		ClientLog::error("Failed to initialize the SDK. Are you sure the correct ManusSDKLibary is used?");
		return ClientReturnCode::ClientReturnCode_FailedToInitialize;
	}


	return ClientReturnCode::ClientReturnCode_Success;
}

ClientReturnCode ManusClient::Connect()
{
	// Integrated mode: always connect locally
	SDKReturnCode t_StartResult = CoreSdk_LookForHosts(1, false);
	if (t_StartResult != SDKReturnCode::SDKReturnCode_Success)
	{
		return ClientReturnCode::ClientReturnCode_FailedToFindHosts;
	}

	uint32_t t_NumberOfHostsFound = 0;
	SDKReturnCode t_NumberResult = CoreSdk_GetNumberOfAvailableHostsFound(&t_NumberOfHostsFound);
	if (t_NumberResult != SDKReturnCode::SDKReturnCode_Success)
	{
		return ClientReturnCode::ClientReturnCode_FailedToFindHosts;
	}

	if (t_NumberOfHostsFound == 0)
	{
		return ClientReturnCode::ClientReturnCode_FailedToFindHosts;
	}

	std::unique_ptr<ManusHost[]> t_AvailableHosts;
	t_AvailableHosts.reset(new ManusHost[t_NumberOfHostsFound]);

	SDKReturnCode t_HostsResult = CoreSdk_GetAvailableHostsFound(t_AvailableHosts.get(), t_NumberOfHostsFound);
	if (t_HostsResult != SDKReturnCode::SDKReturnCode_Success)
	{
		return ClientReturnCode::ClientReturnCode_FailedToFindHosts;
	}

	SDKReturnCode t_ConnectResult = CoreSdk_ConnectToHost(t_AvailableHosts[0]);

	if (t_ConnectResult == SDKReturnCode::SDKReturnCode_NotConnected)
	{
		return ClientReturnCode::ClientReturnCode_FailedToConnect;
	}

	return ClientReturnCode::ClientReturnCode_Success;
}

ClientReturnCode ManusClient::Run()
{
	ClearConsole();

	ClientLog::print("SDK client is running in integrated mode.");

	while (Connect() != ClientReturnCode::ClientReturnCode_Success)
	{
		// not yet connected. wait
		ClientLog::print("minimal client could not connect.trying again in a second.");
		std::this_thread::sleep_for(std::chrono::milliseconds(1000));
	}

	// set the hand motion mode of the RawSkeletonStream. This is optional and can be set to any of the HandMotion enum values. Default = None
	// None will disable hand motion tracking
	const SDKReturnCode t_HandMotionResult = CoreSdk_SetRawSkeletonHandMotion(HandMotion_None);
	if (t_HandMotionResult != SDKReturnCode::SDKReturnCode_Success)
	{
		ClientLog::error("Failed to set hand motion mode. The value returned was {}.", (int32_t)t_HandMotionResult);
	}

	while (m_Running)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(10));

		// Check if left hand calibration needs to be loaded
		if (!m_LeftGloveCalibrationLoaded && m_FirstLeftGloveID != UINT32_MAX)
		{
			LoadGloveCalibration(m_FirstLeftGloveID, "Calibration_left.mcal");
			m_LeftGloveCalibrationLoaded = true;
		}

		// Check if right hand calibration needs to be loaded
		if (!m_RightGloveCalibrationLoaded && m_FirstRightGloveID != UINT32_MAX)
		{
			LoadGloveCalibration(m_FirstRightGloveID, "Calibration_right.mcal");
			m_RightGloveCalibrationLoaded = true;
		}

		if (GetKeyDown(' ')) // press space to exit
		{
			m_Running = false;
		}
	}

	return ClientReturnCode::ClientReturnCode_Success;
}


ClientReturnCode ManusClient::ShutDown()
{
	const SDKReturnCode t_Result = CoreSdk_ShutDown();
	if (t_Result != SDKReturnCode::SDKReturnCode_Success)
	{
		ClientLog::error("Failed to shut down the SDK wrapper. The value returned was {}.", (int32_t)t_Result);
		return ClientReturnCode::ClientReturnCode_FailedToShutDownSDK;
	}

	if (!PlatformSpecificShutdown())
	{
		return ClientReturnCode::ClientReturnCode_FailedPlatformSpecificShutdown;
	}

	return ClientReturnCode::ClientReturnCode_Success;
}

void ManusClient::OnRawSkeletonStreamCallback(const SkeletonStreamInfo* const p_SkeletonStreamInfo)
{
	// ClientLog::info("skeletonsCount: {}", p_SkeletonStreamInfo->skeletonsCount);
	if (s_Instance)
	{
		ClientSkeletonCollection* t_NxtClientSkeleton = new ClientSkeletonCollection();
		t_NxtClientSkeleton->skeletons.resize(p_SkeletonStreamInfo->skeletonsCount);

		proto::GloveState glove_state_msg;

		ManusTimestamp t_Timestamp = p_SkeletonStreamInfo->publishTime;
		uint64_t timestamp = t_Timestamp.time;
		uint64_t sec = timestamp / 1000000000;
		uint64_t nanosec = timestamp % 1000000000;

		std::string id = std::to_string(s_Instance->m_FrameId);
		s_Instance->m_FrameId++;

		proto::Header* header_msg = glove_state_msg.mutable_header();
		proto::Stamp* stamp_msg = header_msg->mutable_stamp();
		stamp_msg->set_sec(sec);
		stamp_msg->set_nanosec(nanosec);
		header_msg->set_frame_id(id);


		for (uint32_t i = 0; i < p_SkeletonStreamInfo->skeletonsCount; i++)
		{
			CoreSdk_GetRawSkeletonInfo(i, &t_NxtClientSkeleton->skeletons[i].info);

			Side t_Side = Side::Side_Invalid;
			std::string t_GloveSide = "Invalid";

			if (t_NxtClientSkeleton->skeletons[i].info.gloveId == s_Instance->m_FirstLeftGloveID){
				t_Side = Side::Side_Left;
				t_GloveSide = "Left";
			}
			else if (t_NxtClientSkeleton->skeletons[i].info.gloveId == s_Instance->m_FirstRightGloveID){
				t_Side = Side::Side_Right;
				t_GloveSide = "Right";
			}
			else{
				continue;
			}
			// Print nodesCount
			t_NxtClientSkeleton->skeletons[i].nodes.resize(t_NxtClientSkeleton->skeletons[i].info.nodesCount);
			t_NxtClientSkeleton->skeletons[i].info.publishTime = p_SkeletonStreamInfo->publishTime;
			CoreSdk_GetRawSkeletonData(i, t_NxtClientSkeleton->skeletons[i].nodes.data(), t_NxtClientSkeleton->skeletons[i].info.nodesCount);
			// ClientLog::info("nodesCount: {}", t_NxtClientSkeleton->skeletons[i].nodes.size());


			if (t_NxtClientSkeleton->skeletons[i].nodes.size() > 0)
			{
				for (uint32_t j = 0; j < t_NxtClientSkeleton->skeletons[i].nodes.size() && j < 25; j++)
				{
					const auto& node = t_NxtClientSkeleton->skeletons[i].nodes[j];

					proto::Pose* hand_pose;
					if (t_Side == Side::Side_Left)
						hand_pose = glove_state_msg.add_left_mocap_pose();
					else
						hand_pose = glove_state_msg.add_right_mocap_pose();

					proto::Position* pos = hand_pose->mutable_position();
					pos->set_x(node.transform.position.x);
					pos->set_y(node.transform.position.y);
					pos->set_z(node.transform.position.z);

					proto::Quaternion* ori = hand_pose->mutable_orientation();
					ori->set_w(node.transform.rotation.w);
					ori->set_x(node.transform.rotation.x);
					ori->set_y(node.transform.rotation.y);
					ori->set_z(node.transform.rotation.z);
				}

				auto now = std::chrono::system_clock::now();
				auto duration = now.time_since_epoch();
				auto sys_sec = std::chrono::duration_cast<std::chrono::seconds>(duration).count();
				auto sys_nanosec = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count() % 1000000000;

				// Print every 100 frames
				if (s_Instance->m_FrameId % 100 == 0)
				{
					ClientLog::info("Frame[{}] glove: {} is published. - System time: {}.{}s",
						std::to_string(s_Instance->m_FrameId),
						t_GloveSide,
						std::to_string(sys_sec),
						std::to_string(sys_nanosec));
				}
			}
		}

		std::string serialized_data;
		glove_state_msg.SerializeToString(&serialized_data);
		zmq::message_t zmq_msg(serialized_data.size());
		memcpy(zmq_msg.data(), serialized_data.data(), serialized_data.size());

		s_Instance->m_ZmqPublisher->send(zmq_msg, zmq::send_flags::none);
	}
}

void ManusClient::OnLandscapeCallback(const Landscape* const p_Landscape)
{
	if (s_Instance == nullptr)return;
	// if (s_Instance->m_Landscape != nullptr) return;
	Landscape* t_Landscape = new Landscape(*p_Landscape);
	s_Instance->m_Landscape = t_Landscape;

	for (size_t i = 0; i < s_Instance->m_Landscape->gloveDevices.gloveCount; i++)
	{
		// ClientLog::info("First glove id {} and side {} and glove count {}", s_Instance->m_Landscape->gloveDevices.gloves[i].id, s_Instance->m_Landscape->gloveDevices.gloves[i].side, s_Instance->m_Landscape->gloveDevices.gloveCount);
		if (s_Instance->m_Landscape->gloveDevices.gloves[i].side == Side::Side_Left)
		{
			s_Instance->m_FirstLeftGloveID = s_Instance->m_Landscape->gloveDevices.gloves[i].id;
			// ClientLog::info("First left glove ID: {}", s_Instance->m_FirstLeftGloveID);
			continue;
		}
		if (s_Instance->m_Landscape->gloveDevices.gloves[i].side == Side::Side_Right)
		{
			s_Instance->m_FirstRightGloveID = s_Instance->m_Landscape->gloveDevices.gloves[i].id;
			// ClientLog::info("First right glove ID: {}", s_Instance->m_FirstRightGloveID);
			continue;
		}
	}
}

ClientReturnCode ManusClient::RegisterAllCallbacks()
{
	// Register the callback to receive Raw Skeleton data
	// it is optional, but without it you can not see any resulting skeleton data.
	// see OnRawSkeletonStreamCallback for more details.
	const SDKReturnCode t_RegisterSkeletonCallbackResult = CoreSdk_RegisterCallbackForRawSkeletonStream(*OnRawSkeletonStreamCallback);
	if (t_RegisterSkeletonCallbackResult != SDKReturnCode::SDKReturnCode_Success)
	{
		ClientLog::error("Failed to register callback function for processing skeletal data from Manus Core. The value returned was {}.", (int32_t)t_RegisterSkeletonCallbackResult);
		return ClientReturnCode::ClientReturnCode_FailedToInitialize;
	}

	// Register the callback to receive Landscape data
	const SDKReturnCode t_RegisterLandscapeCallbackResult = CoreSdk_RegisterCallbackForLandscapeStream(*OnLandscapeCallback);
	if (t_RegisterLandscapeCallbackResult != SDKReturnCode::SDKReturnCode_Success)
	{
		ClientLog::error("Failed to register callback for landscape from Manus Core. The value returned was {}.", (int32_t)t_RegisterLandscapeCallbackResult);
		return ClientReturnCode::ClientReturnCode_FailedToInitialize;
	}

	return ClientReturnCode::ClientReturnCode_Success;
}

ClientReturnCode ManusClient::InitializeSDK()
{
	// Use Integrated mode directly, no user input required
	SDKReturnCode t_InitializeResult = CoreSdk_InitializeIntegrated();

	if (t_InitializeResult != SDKReturnCode::SDKReturnCode_Success)
	{
		ClientLog::error("Failed to initialize the Manus Core SDK. The value returned was {}.", (int32_t)t_InitializeResult);
		return ClientReturnCode::ClientReturnCode_FailedToInitialize;
	}

	const ClientReturnCode t_CallBackResults = RegisterAllCallbacks();
	if (t_CallBackResults != ::ClientReturnCode::ClientReturnCode_Success)
	{
		ClientLog::error("Failed to initialize callbacks.");
		return t_CallBackResults;
	}

	CoordinateSystemVUH t_VUH;
	CoordinateSystemVUH_Init(&t_VUH);
	t_VUH.handedness = Side::Side_Right;
	t_VUH.up = AxisPolarity::AxisPolarity_PositiveZ;
	t_VUH.view = AxisView::AxisView_XFromViewer;
	t_VUH.unitScale = 1.0f;

	const SDKReturnCode t_CoordinateResult = CoreSdk_InitializeCoordinateSystemWithVUH(t_VUH, true);

	if (t_CoordinateResult != SDKReturnCode::SDKReturnCode_Success)
	{
		ClientLog::error("Failed to initialize the Manus Core SDK coordinate system. The value returned was {}.", (int32_t)t_CoordinateResult);
		return ClientReturnCode::ClientReturnCode_FailedToInitialize;
	}

	return ClientReturnCode::ClientReturnCode_Success;
}

void ManusClient::LoadGloveCalibration(uint32_t p_GloveId, const std::string& p_CalibrationFileName)
{
	// Get current working directory
	std::string t_CurrentDirectory = std::filesystem::current_path().string();

	// Build full path to calibration file
	std::string t_CalibrationFilePath = t_CurrentDirectory + s_SlashForFilesystemPath + p_CalibrationFileName;

	// Check if file exists
	if (!DoesFolderOrFileExist(t_CalibrationFilePath))
	{
		ClientLog::warn("Calibration file does not exist: {}", t_CalibrationFilePath);
		return;
	}

	// Read file
	std::ifstream t_File(t_CalibrationFilePath, std::ios::binary);
	if (!t_File)
	{
		ClientLog::warn("Unable to open calibration file: {}", t_CalibrationFilePath);
		return;
	}

	// Get file size
	t_File.seekg(0, t_File.end);
	int t_FileLength = (int)t_File.tellg();
	t_File.seekg(0, t_File.beg);

	if (t_FileLength <= 0)
	{
		ClientLog::warn("Calibration file is empty: {}", t_CalibrationFilePath);
		t_File.close();
		return;
	}

	// Read calibration data
	unsigned char* t_CalibrationData = new unsigned char[t_FileLength];
	t_File.read((char*)t_CalibrationData, t_FileLength);
	t_File.close();

	// Set glove calibration
	SetGloveCalibrationReturnCode t_Result;
	SDKReturnCode t_SetResult = CoreSdk_SetGloveCalibration(p_GloveId, t_CalibrationData, t_FileLength, &t_Result);

	// Clean up memory
	delete[] t_CalibrationData;

	// Print result
	if (t_SetResult == SDKReturnCode::SDKReturnCode_Success)
	{
		ClientLog::info("Successfully loaded glove calibration file: {} (Glove ID: {})", p_CalibrationFileName, p_GloveId);
		ClientLog::info("Calibration setting result: {}", (int)t_Result);
	}
	else
	{
		ClientLog::error("Failed to load glove calibration file: {} (Glove ID: {}), SDK return code: {}",
			p_CalibrationFileName, p_GloveId, (int)t_SetResult);
	}
}



int main(int argc, char* argv[])
{
	ManusSDK::ClientLog::print("Starting SDK client!");

	ClientReturnCode t_Result;
	ManusClient t_Client;

	t_Result = t_Client.Initialize();

	if (t_Result != ClientReturnCode::ClientReturnCode_Success)
	{
		t_Client.ShutDown();
		return static_cast<int>(t_Result);
	}
	ManusSDK::ClientLog::print("SDK client is initialized.");

	t_Result = t_Client.Run();
	if (t_Result != ClientReturnCode::ClientReturnCode_Success)
	{
		t_Client.ShutDown();
		return static_cast<int>(t_Result);
	}

	ManusSDK::ClientLog::print("SDK client is done, shutting down.");
	t_Result = t_Client.ShutDown();
	return static_cast<int>(t_Result);
}
