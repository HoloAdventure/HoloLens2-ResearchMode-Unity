#include "pch.h"
#include "HL2ResearchMode.h"
#include "HL2ResearchMode.g.cpp"

extern "C"
HMODULE LoadLibraryA(
    LPCSTR lpLibFileName
);

static ResearchModeSensorConsent camAccessCheck;
static HANDLE camConsentGiven;

using namespace DirectX;
using namespace winrt::Windows::Perception;
using namespace winrt::Windows::Perception::Spatial;
using namespace winrt::Windows::Perception::Spatial::Preview;

typedef std::chrono::duration<int64_t, std::ratio<1, 10'000'000>> HundredsOfNanoseconds;

namespace winrt::HL2UnityPlugin::implementation
{
    HL2ResearchMode::HL2ResearchMode() 
    {
        // Load Research Mode library
        camConsentGiven = CreateEvent(nullptr, true, false, nullptr);
        HMODULE hrResearchMode = LoadLibraryA("ResearchModeAPI");
        HRESULT hr = S_OK;

        if (hrResearchMode)
        {
            typedef HRESULT(__cdecl* PFN_CREATEPROVIDER) (IResearchModeSensorDevice** ppSensorDevice);
            PFN_CREATEPROVIDER pfnCreate = reinterpret_cast<PFN_CREATEPROVIDER>(GetProcAddress(hrResearchMode, "CreateResearchModeSensorDevice"));
            if (pfnCreate)
            {
                winrt::check_hresult(pfnCreate(&m_pSensorDevice));
            }
            else
            {
                winrt::check_hresult(E_INVALIDARG);
            }
        }

        // get spatial locator of rigNode
        GUID guid;
        IResearchModeSensorDevicePerception* pSensorDevicePerception;
        winrt::check_hresult(m_pSensorDevice->QueryInterface(IID_PPV_ARGS(&pSensorDevicePerception)));
        winrt::check_hresult(pSensorDevicePerception->GetRigNodeId(&guid));
        pSensorDevicePerception->Release();
        m_locator = SpatialGraphInteropPreview::CreateLocatorForNode(guid);

        size_t sensorCount = 0;

        winrt::check_hresult(m_pSensorDevice->QueryInterface(IID_PPV_ARGS(&m_pSensorDeviceConsent)));
        winrt::check_hresult(m_pSensorDeviceConsent->RequestCamAccessAsync(HL2ResearchMode::CamAccessOnComplete));

        m_pSensorDevice->DisableEyeSelection();

        winrt::check_hresult(m_pSensorDevice->GetSensorCount(&sensorCount));
        m_sensorDescriptors.resize(sensorCount);
        winrt::check_hresult(m_pSensorDevice->GetSensorDescriptors(m_sensorDescriptors.data(), m_sensorDescriptors.size(), &sensorCount));
    }

    void HL2ResearchMode::InitializeDepthSensor() 
    {
       
        for (auto sensorDescriptor : m_sensorDescriptors)
        {
            if (sensorDescriptor.sensorType == DEPTH_AHAT)
            {
                winrt::check_hresult(m_pSensorDevice->GetSensor(sensorDescriptor.sensorType, &m_depthSensor));
                winrt::check_hresult(m_depthSensor->QueryInterface(IID_PPV_ARGS(&m_pDepthCameraSensor)));
                winrt::check_hresult(m_pDepthCameraSensor->GetCameraExtrinsicsMatrix(&m_depthCameraPose));
                m_depthCameraPoseInvMatrix = XMMatrixInverse(nullptr, XMLoadFloat4x4(&m_depthCameraPose));
                break;
            }
        }
    }

    void HL2ResearchMode::InitializeLongDepthSensor()
    {
        for (auto sensorDescriptor : m_sensorDescriptors)
        {
            if (sensorDescriptor.sensorType == DEPTH_LONG_THROW)
            {
                winrt::check_hresult(m_pSensorDevice->GetSensor(sensorDescriptor.sensorType, &m_longDepthSensor));
                winrt::check_hresult(m_longDepthSensor->QueryInterface(IID_PPV_ARGS(&m_pLongDepthCameraSensor)));
                winrt::check_hresult(m_pLongDepthCameraSensor->GetCameraExtrinsicsMatrix(&m_longDepthCameraPose));
                m_longDepthCameraPoseInvMatrix = XMMatrixInverse(nullptr, XMLoadFloat4x4(&m_longDepthCameraPose));
                break;
            }
        }
    }

    void HL2ResearchMode::InitializeSpatialCamerasFront()
    {
        for (auto sensorDescriptor : m_sensorDescriptors)
        {
            if (sensorDescriptor.sensorType == LEFT_FRONT)
            {
                winrt::check_hresult(m_pSensorDevice->GetSensor(sensorDescriptor.sensorType, &m_LFSensor));
                winrt::check_hresult(m_LFSensor->QueryInterface(IID_PPV_ARGS(&m_LFCameraSensor)));
                winrt::check_hresult(m_LFCameraSensor->GetCameraExtrinsicsMatrix(&m_LFCameraPose));
                m_LFCameraPoseInvMatrix = XMMatrixInverse(nullptr, XMLoadFloat4x4(&m_LFCameraPose));
            }
            if (sensorDescriptor.sensorType == RIGHT_FRONT)
            {
                winrt::check_hresult(m_pSensorDevice->GetSensor(sensorDescriptor.sensorType, &m_RFSensor));
                winrt::check_hresult(m_RFSensor->QueryInterface(IID_PPV_ARGS(&m_RFCameraSensor)));
                winrt::check_hresult(m_RFCameraSensor->GetCameraExtrinsicsMatrix(&m_RFCameraPose));
                m_RFCameraPoseInvMatrix = XMMatrixInverse(nullptr, XMLoadFloat4x4(&m_RFCameraPose));
            }
        }
    }

    void HL2ResearchMode::StartDepthSensorLoop() 
    {
        //std::thread th1([this] {this->DepthSensorLoopTest(); });
        if (m_refFrame == nullptr) 
        {
            m_refFrame = m_locator.GetDefault().CreateStationaryFrameOfReferenceAtCurrentLocation().CoordinateSystem();
        }

        m_pDepthUpdateThread = new std::thread(HL2ResearchMode::DepthSensorLoop, this);
    }

    void HL2ResearchMode::DepthSensorLoop(HL2ResearchMode* pHL2ResearchMode)
    {
        // prevent starting loop for multiple times
        if (!pHL2ResearchMode->m_depthSensorLoopStarted)
        {
            pHL2ResearchMode->m_depthSensorLoopStarted = true;
        }
        else {
            return;
        }

        pHL2ResearchMode->m_depthSensor->OpenStream();

        try 
        {
            while (pHL2ResearchMode->m_depthSensorLoopStarted)
            {
                IResearchModeSensorFrame* pDepthSensorFrame = nullptr;
                ResearchModeSensorResolution resolution;
                pHL2ResearchMode->m_depthSensor->GetNextBuffer(&pDepthSensorFrame);

                // process sensor frame
                pDepthSensorFrame->GetResolution(&resolution);
                pHL2ResearchMode->m_depthResolution = resolution;
                
                IResearchModeSensorDepthFrame* pDepthFrame = nullptr;
                winrt::check_hresult(pDepthSensorFrame->QueryInterface(IID_PPV_ARGS(&pDepthFrame)));

                size_t outBufferCount = 0;
                const UINT16* pDepth = nullptr;
                pDepthFrame->GetBuffer(&pDepth, &outBufferCount);
                pHL2ResearchMode->m_depthBufferSize = outBufferCount;
                size_t outAbBufferCount = 0;
                const UINT16* pAbImage = nullptr;
                pDepthFrame->GetAbDepthBuffer(&pAbImage, &outAbBufferCount);

                auto pDepthTexture = std::make_unique<uint8_t[]>(outBufferCount);
                auto pAbTexture = std::make_unique<uint8_t[]>(outAbBufferCount);
                std::vector<float> pointCloud;

                // get tracking transform
                ResearchModeSensorTimestamp timestamp;
                pDepthSensorFrame->GetTimeStamp(&timestamp);

                auto ts = PerceptionTimestampHelper::FromSystemRelativeTargetTime(HundredsOfNanoseconds(checkAndConvertUnsigned(timestamp.HostTicks)));
                auto transToWorld = pHL2ResearchMode->m_locator.TryLocateAtTimestamp(ts, pHL2ResearchMode->m_refFrame);
                if (transToWorld == nullptr)
                {
                    continue;
                }
                auto rot = transToWorld.Orientation();
                /*{
                    std::stringstream ss;
                    ss << rot.x << "," << rot.y << "," << rot.z << "," << rot.w << "\n";
                    std::string msg = ss.str();
                    std::wstring widemsg = std::wstring(msg.begin(), msg.end());
                    OutputDebugString(widemsg.c_str());
                }*/
                auto quatInDx = XMFLOAT4(rot.x, rot.y, rot.z, rot.w);
                auto rotMat = XMMatrixRotationQuaternion(XMLoadFloat4(&quatInDx));
                auto pos = transToWorld.Position();
                auto posMat = XMMatrixTranslation(pos.x, pos.y, pos.z);
                auto depthToWorld = pHL2ResearchMode->m_depthCameraPoseInvMatrix * rotMat * posMat;

                pHL2ResearchMode->mu.lock();
                auto roiCenterFloat = XMFLOAT3(pHL2ResearchMode->m_roiCenter[0], pHL2ResearchMode->m_roiCenter[1], pHL2ResearchMode->m_roiCenter[2]);
                auto roiBoundFloat = XMFLOAT3(pHL2ResearchMode->m_roiBound[0], pHL2ResearchMode->m_roiBound[1], pHL2ResearchMode->m_roiBound[2]);
                pHL2ResearchMode->mu.unlock();
                XMVECTOR roiCenter = XMLoadFloat3(&roiCenterFloat);
                XMVECTOR roiBound = XMLoadFloat3(&roiBoundFloat);
                
                UINT16 maxAbValue = 0;
                for (UINT i = 0; i < resolution.Height; i++)
                {
                    for (UINT j = 0; j < resolution.Width; j++)
                    {
                        auto idx = resolution.Width * i + j;
                        UINT16 depth = pDepth[idx];
                        depth = (depth > 4090) ? 0 : depth - pHL2ResearchMode->m_depthOffset;

                        // back-project point cloud within Roi
                        if (i > pHL2ResearchMode->depthCamRoi.kRowLower*resolution.Height&& i < pHL2ResearchMode->depthCamRoi.kRowUpper * resolution.Height &&
                            j > pHL2ResearchMode->depthCamRoi.kColLower* resolution.Width&& j < pHL2ResearchMode->depthCamRoi.kColUpper * resolution.Width &&
                            depth > pHL2ResearchMode->depthCamRoi.depthNearClip && depth < pHL2ResearchMode->depthCamRoi.depthFarClip)
                        {
                            float xy[2] = { 0, 0 };
                            float uv[2] = { j, i };
                            pHL2ResearchMode->m_pDepthCameraSensor->MapImagePointToCameraUnitPlane(uv, xy);
                            auto pointOnUnitPlane = XMFLOAT3(xy[0], xy[1], 1);
                            auto tempPoint = (float)depth / 1000 * XMVector3Normalize(XMLoadFloat3(&pointOnUnitPlane));
                            // apply transformation
                            auto pointInWorld = XMVector3Transform(tempPoint, depthToWorld);

                            // filter point cloud based on region of interest
                            if (!pHL2ResearchMode->m_useRoiFilter ||
                                (pHL2ResearchMode->m_useRoiFilter && XMVector3InBounds(pointInWorld - roiCenter, roiBound)))
                            {
                                pointCloud.push_back(XMVectorGetX(pointInWorld));
                                pointCloud.push_back(XMVectorGetY(pointInWorld));
                                pointCloud.push_back(-XMVectorGetZ(pointInWorld));
                            }
                        }

                        // save depth map as grayscale texture pixel into temp buffer
                        if (depth == 0) { pDepthTexture.get()[idx] = 0; }
                        else { pDepthTexture.get()[idx] = (uint8_t)((float)depth / 1000 * 255); }

                        // save AbImage as grayscale texture pixel into temp buffer
                        UINT16 abValue = pAbImage[idx];
                        uint8_t processedAbValue = 0;
                        if (abValue > 1000) { processedAbValue = 0xFF; }
                        else { processedAbValue = (uint8_t)((float)abValue / 1000 * 255); }

                        /*if (abValue > maxAbValue) 
                        {
                            maxAbValue = abValue;
                            std::stringstream ss;
                            ss << "Non zero valule. Idx: " << idx << " Raw Value: " << abValue << "Processed: " << (int)processedAbValue << "\n";
                            std::string msg = ss.str();
                            std::wstring widemsg = std::wstring(msg.begin(), msg.end());
                            OutputDebugString(widemsg.c_str());
                        }*/
                        pAbTexture.get()[idx] = processedAbValue;

                        // save the depth of center pixel
                        if (i == (UINT)(0.35 * resolution.Height) && j == (UINT)(0.5 * resolution.Width)
                            && pointCloud.size()>=3)
                        {
                            pHL2ResearchMode->m_centerDepth = depth;
                            if (depth > pHL2ResearchMode->depthCamRoi.depthNearClip && depth < pHL2ResearchMode->depthCamRoi.depthFarClip)
                            {
                                std::lock_guard<std::mutex> l(pHL2ResearchMode->mu);
                                pHL2ResearchMode->m_centerPoint[0] = *(pointCloud.end() - 3);
                                pHL2ResearchMode->m_centerPoint[1] = *(pointCloud.end() - 2);
                                pHL2ResearchMode->m_centerPoint[2] = *(pointCloud.end() - 1);
                            }
                        }
                    }
                }

                // save data
                {
                    std::lock_guard<std::mutex> l(pHL2ResearchMode->mu);

                    // save point cloud
                    if (!pHL2ResearchMode->m_pointCloud)
                    {
                        OutputDebugString(L"Create Space for point cloud...\n");
                        pHL2ResearchMode->m_pointCloud = new float[outBufferCount * 3];
                    }

                    memcpy(pHL2ResearchMode->m_pointCloud, pointCloud.data(), pointCloud.size() * sizeof(float));
                    pHL2ResearchMode->m_pointcloudLength = pointCloud.size();

                    // save raw depth map
                    if (!pHL2ResearchMode->m_depthMap)
                    {
                        OutputDebugString(L"Create Space for depth map...\n");
                        pHL2ResearchMode->m_depthMap = new UINT16[outBufferCount];
                    }
                    memcpy(pHL2ResearchMode->m_depthMap, pDepth, outBufferCount * sizeof(UINT16));

                    // save pre-processed depth map texture (for visualization)
                    if (!pHL2ResearchMode->m_depthMapTexture)
                    {
                        OutputDebugString(L"Create Space for depth map texture...\n");
                        pHL2ResearchMode->m_depthMapTexture = new UINT8[outBufferCount];
                    }
                    memcpy(pHL2ResearchMode->m_depthMapTexture, pDepthTexture.get(), outBufferCount * sizeof(UINT8));

                    // save raw AbImage
                    if (!pHL2ResearchMode->m_shortAbImage)
                    {
                        OutputDebugString(L"Create Space for short AbImage...\n");
                        pHL2ResearchMode->m_shortAbImage = new UINT16[outBufferCount];
                    }
                    memcpy(pHL2ResearchMode->m_shortAbImage, pAbImage, outBufferCount * sizeof(UINT16));

                    // save pre-processed AbImage texture (for visualization)
                    if (!pHL2ResearchMode->m_shortAbImageTexture)
                    {
                        OutputDebugString(L"Create Space for short AbImage texture...\n");
                        pHL2ResearchMode->m_shortAbImageTexture = new UINT8[outBufferCount];
                    }
                    memcpy(pHL2ResearchMode->m_shortAbImageTexture, pAbTexture.get(), outBufferCount * sizeof(UINT8));
                }
                pHL2ResearchMode->m_shortAbImageTextureUpdated = true;
                pHL2ResearchMode->m_depthMapTextureUpdated = true;
                pHL2ResearchMode->m_pointCloudUpdated = true;

                pDepthTexture.reset();

                // release space
                if (pDepthFrame) {
                    pDepthFrame->Release();
                }
                if (pDepthSensorFrame)
                {
                    pDepthSensorFrame->Release();
                }
                
            }
        }
        catch (...)  {}
        pHL2ResearchMode->m_depthSensor->CloseStream();
        pHL2ResearchMode->m_depthSensor->Release();
        pHL2ResearchMode->m_depthSensor = nullptr;
        
    }

    void HL2ResearchMode::StartLongDepthSensorLoop()
    {
        if (m_refFrame == nullptr)
        {
            m_refFrame = m_locator.GetDefault().CreateStationaryFrameOfReferenceAtCurrentLocation().CoordinateSystem();
        }

        m_pLongDepthUpdateThread = new std::thread(HL2ResearchMode::LongDepthSensorLoop, this);
    }

    void HL2ResearchMode::LongDepthSensorLoop(HL2ResearchMode* pHL2ResearchMode)
    {
        // prevent starting loop for multiple times
        if (!pHL2ResearchMode->m_longDepthSensorLoopStarted)
        {
            pHL2ResearchMode->m_longDepthSensorLoopStarted = true;
        }
        else {
            return;
        }

        pHL2ResearchMode->m_longDepthSensor->OpenStream();

        try
        {
            while (pHL2ResearchMode->m_longDepthSensorLoopStarted)
            {
                IResearchModeSensorFrame* pDepthSensorFrame = nullptr;
                ResearchModeSensorResolution resolution;
                pHL2ResearchMode->m_longDepthSensor->GetNextBuffer(&pDepthSensorFrame);

                // process sensor frame
                pDepthSensorFrame->GetResolution(&resolution);
                pHL2ResearchMode->m_longDepthResolution = resolution;

                IResearchModeSensorDepthFrame* pDepthFrame = nullptr;
                winrt::check_hresult(pDepthSensorFrame->QueryInterface(IID_PPV_ARGS(&pDepthFrame)));

                size_t outBufferCount = 0;
                const UINT16* pDepth = nullptr;

                const BYTE* pSigma = nullptr;
                pDepthFrame->GetSigmaBuffer(&pSigma, &outBufferCount);
                pDepthFrame->GetBuffer(&pDepth, &outBufferCount);
                pHL2ResearchMode->m_longDepthBufferSize = outBufferCount;

                auto pDepthTexture = std::make_unique<uint8_t[]>(outBufferCount);

                // get tracking transform
                ResearchModeSensorTimestamp timestamp;
                pDepthSensorFrame->GetTimeStamp(&timestamp);

                auto ts = PerceptionTimestampHelper::FromSystemRelativeTargetTime(HundredsOfNanoseconds(checkAndConvertUnsigned(timestamp.HostTicks)));
                auto transToWorld = pHL2ResearchMode->m_locator.TryLocateAtTimestamp(ts, pHL2ResearchMode->m_refFrame);
                if (transToWorld == nullptr)
                {
                    continue;
                }

                for (UINT i = 0; i < resolution.Height; i++)
                {
                    for (UINT j = 0; j < resolution.Width; j++)
                    {
                        auto idx = resolution.Width * i + j;
                        UINT16 depth = pDepth[idx];
                        depth = (pSigma[idx] & 0x80) ? 0 : depth - pHL2ResearchMode->m_depthOffset;

                        // save as grayscale texture pixel into temp buffer
                        if (depth == 0) { pDepthTexture.get()[idx] = 0; }
                        else { pDepthTexture.get()[idx] = (uint8_t)((float)depth / 4000 * 255); }
                    }
                }

                // save data
                {
                    std::lock_guard<std::mutex> l(pHL2ResearchMode->mu);

                    // save raw depth map
                    if (!pHL2ResearchMode->m_longDepthMap)
                    {
                        OutputDebugString(L"Create Space for depth map...\n");
                        pHL2ResearchMode->m_longDepthMap = new UINT16[outBufferCount];
                    }
                    memcpy(pHL2ResearchMode->m_longDepthMap, pDepth, outBufferCount * sizeof(UINT16));

                    // save pre-processed depth map texture (for visualization)
                    if (!pHL2ResearchMode->m_longDepthMapTexture)
                    {
                        OutputDebugString(L"Create Space for depth map texture...\n");
                        pHL2ResearchMode->m_longDepthMapTexture = new UINT8[outBufferCount];
                    }
                    memcpy(pHL2ResearchMode->m_longDepthMapTexture, pDepthTexture.get(), outBufferCount * sizeof(UINT8));
                }

                pHL2ResearchMode->m_longDepthMapTextureUpdated = true;

                pDepthTexture.reset();

                // release space
                if (pDepthFrame) {
                    pDepthFrame->Release();
                }
                if (pDepthSensorFrame)
                {
                    pDepthSensorFrame->Release();
                }

            }
        }
        catch (...) {}
        pHL2ResearchMode->m_longDepthSensor->CloseStream();
        pHL2ResearchMode->m_longDepthSensor->Release();
        pHL2ResearchMode->m_longDepthSensor = nullptr;

    }

    void HL2ResearchMode::StartSpatialCamerasFrontLoop()
    {
        if (m_refFrame == nullptr)
        {
            m_refFrame = m_locator.GetDefault().CreateStationaryFrameOfReferenceAtCurrentLocation().CoordinateSystem();
        }

        m_pSpatialCamerasFrontUpdateThread = new std::thread(HL2ResearchMode::SpatialCamerasFrontLoop, this);
    }

    void HL2ResearchMode::SpatialCamerasFrontLoop(HL2ResearchMode* pHL2ResearchMode)
    {
        // prevent starting loop for multiple times
        if (!pHL2ResearchMode->m_spatialCamerasFrontLoopStarted)
        {
            pHL2ResearchMode->m_spatialCamerasFrontLoopStarted = true;
        }
        else {
            return;
        }

        pHL2ResearchMode->m_LFSensor->OpenStream();
        pHL2ResearchMode->m_RFSensor->OpenStream();

        try
        {
            while (pHL2ResearchMode->m_spatialCamerasFrontLoopStarted)
            {
                IResearchModeSensorFrame* pLFCameraFrame = nullptr;
                IResearchModeSensorFrame* pRFCameraFrame = nullptr;
                ResearchModeSensorResolution LFResolution;
                ResearchModeSensorResolution RFResolution;
                pHL2ResearchMode->m_LFSensor->GetNextBuffer(&pLFCameraFrame);
				pHL2ResearchMode->m_RFSensor->GetNextBuffer(&pRFCameraFrame);

                // process sensor frame
                pLFCameraFrame->GetResolution(&LFResolution);
                pHL2ResearchMode->m_LFResolution = LFResolution;
                pRFCameraFrame->GetResolution(&RFResolution);
                pHL2ResearchMode->m_RFResolution = RFResolution;

                IResearchModeSensorVLCFrame* pLFFrame = nullptr;
                winrt::check_hresult(pLFCameraFrame->QueryInterface(IID_PPV_ARGS(&pLFFrame)));
                IResearchModeSensorVLCFrame* pRFFrame = nullptr;
                winrt::check_hresult(pRFCameraFrame->QueryInterface(IID_PPV_ARGS(&pRFFrame)));

                size_t LFOutBufferCount = 0;
                const BYTE *pLFImage = nullptr;
                pLFFrame->GetBuffer(&pLFImage, &LFOutBufferCount);
                pHL2ResearchMode->m_LFbufferSize = LFOutBufferCount;
				size_t RFOutBufferCount = 0;
				const BYTE *pRFImage = nullptr;
				pRFFrame->GetBuffer(&pRFImage, &RFOutBufferCount);
				pHL2ResearchMode->m_RFbufferSize = RFOutBufferCount;

                // get tracking transform
                ResearchModeSensorTimestamp timestamp;
                pLFCameraFrame->GetTimeStamp(&timestamp);

                auto ts = PerceptionTimestampHelper::FromSystemRelativeTargetTime(HundredsOfNanoseconds(checkAndConvertUnsigned(timestamp.HostTicks)));
                auto transToWorld = pHL2ResearchMode->m_locator.TryLocateAtTimestamp(ts, pHL2ResearchMode->m_refFrame);
                if (transToWorld == nullptr)
                {
                    continue;
                }
                auto rot = transToWorld.Orientation();
                /*{
                    std::stringstream ss;
                    ss << rot.x << "," << rot.y << "," << rot.z << "," << rot.w << "\n";
                    std::string msg = ss.str();
                    std::wstring widemsg = std::wstring(msg.begin(), msg.end());
                    OutputDebugString(widemsg.c_str());
                }*/
                auto quatInDx = XMFLOAT4(rot.x, rot.y, rot.z, rot.w);
                auto rotMat = XMMatrixRotationQuaternion(XMLoadFloat4(&quatInDx));
                auto pos = transToWorld.Position();
                auto posMat = XMMatrixTranslation(pos.x, pos.y, pos.z);
                auto LfToWorld = pHL2ResearchMode->m_LFCameraPoseInvMatrix * rotMat * posMat;
				auto RfToWorld = pHL2ResearchMode->m_RFCameraPoseInvMatrix * rotMat * posMat;

                // save data
                {
                    std::lock_guard<std::mutex> l(pHL2ResearchMode->mu);

					// save LF and RF images
					if (!pHL2ResearchMode->m_LFImage)
					{
						OutputDebugString(L"Create Space for Left Front Image...\n");
						pHL2ResearchMode->m_LFImage = new UINT8[LFOutBufferCount];
					}
					memcpy(pHL2ResearchMode->m_LFImage, pLFImage, LFOutBufferCount * sizeof(UINT8));

					if (!pHL2ResearchMode->m_RFImage)
					{
						OutputDebugString(L"Create Space for Right Front Image...\n");
						pHL2ResearchMode->m_RFImage = new UINT8[RFOutBufferCount];
					}
					memcpy(pHL2ResearchMode->m_RFImage, pRFImage, RFOutBufferCount * sizeof(UINT8));
                }
				pHL2ResearchMode->m_LFImageUpdated = true;
				pHL2ResearchMode->m_RFImageUpdated = true;

                // release space
				if (pLFFrame) pLFFrame->Release();
				if (pRFFrame) pRFFrame->Release();

				if (pLFCameraFrame) pLFCameraFrame->Release();
				if (pRFCameraFrame) pRFCameraFrame->Release();
            }
        }
        catch (...) {}
        pHL2ResearchMode->m_LFSensor->CloseStream();
        pHL2ResearchMode->m_LFSensor->Release();
        pHL2ResearchMode->m_LFSensor = nullptr;

		pHL2ResearchMode->m_RFSensor->CloseStream();
		pHL2ResearchMode->m_RFSensor->Release();
		pHL2ResearchMode->m_RFSensor = nullptr;
    }

    void HL2ResearchMode::CamAccessOnComplete(ResearchModeSensorConsent consent)
    {
        camAccessCheck = consent;
        SetEvent(camConsentGiven);
    }

    inline UINT16 HL2ResearchMode::GetCenterDepth() {return m_centerDepth;}

    inline int HL2ResearchMode::GetDepthBufferSize() { return m_depthBufferSize; }

    inline bool HL2ResearchMode::DepthMapTextureUpdated() { return m_depthMapTextureUpdated; }

    inline bool HL2ResearchMode::ShortAbImageTextureUpdated() { return m_shortAbImageTextureUpdated; }

    inline bool HL2ResearchMode::PointCloudUpdated() { return m_pointCloudUpdated; }

    inline int HL2ResearchMode::GetLongDepthBufferSize() { return m_longDepthBufferSize; }

    inline bool HL2ResearchMode::LongDepthMapTextureUpdated() { return m_longDepthMapTextureUpdated; }

	inline bool HL2ResearchMode::LFImageUpdated() { return m_LFImageUpdated; }

	inline bool HL2ResearchMode::RFImageUpdated() { return m_RFImageUpdated; }

    hstring HL2ResearchMode::PrintDepthResolution()
    {
        std::string res_c_ctr = std::to_string(m_depthResolution.Height) + "x" + std::to_string(m_depthResolution.Width) + "x" + std::to_string(m_depthResolution.BytesPerPixel);
        return winrt::to_hstring(res_c_ctr);
    }

    hstring HL2ResearchMode::PrintDepthExtrinsics()
    {
        std::stringstream ss;
        ss << "Extrinsics: \n" << MatrixToString(m_depthCameraPose);
        std::string msg = ss.str();
        std::wstring widemsg = std::wstring(msg.begin(), msg.end());
        OutputDebugString(widemsg.c_str());
        return winrt::to_hstring(msg);
    }

	hstring HL2ResearchMode::PrintLFResolution()
	{
		std::string res_c_ctr = std::to_string(m_LFResolution.Height) + "x" + std::to_string(m_LFResolution.Width) + "x" + std::to_string(m_LFResolution.BytesPerPixel);
		return winrt::to_hstring(res_c_ctr);
	}

	hstring HL2ResearchMode::PrintLFExtrinsics()
	{
		std::stringstream ss;
		ss << "Extrinsics: \n" << MatrixToString(m_LFCameraPose);
		std::string msg = ss.str();
		std::wstring widemsg = std::wstring(msg.begin(), msg.end());
		OutputDebugString(widemsg.c_str());
		return winrt::to_hstring(msg);
	}

	hstring HL2ResearchMode::PrintRFResolution()
	{
		std::string res_c_ctr = std::to_string(m_RFResolution.Height) + "x" + std::to_string(m_RFResolution.Width) + "x" + std::to_string(m_RFResolution.BytesPerPixel);
		return winrt::to_hstring(res_c_ctr);
	}

	hstring HL2ResearchMode::PrintRFExtrinsics()
	{
		std::stringstream ss;
		ss << "Extrinsics: \n" << MatrixToString(m_RFCameraPose);
		std::string msg = ss.str();
		std::wstring widemsg = std::wstring(msg.begin(), msg.end());
		OutputDebugString(widemsg.c_str());
		return winrt::to_hstring(msg);
	}

    std::string HL2ResearchMode::MatrixToString(DirectX::XMFLOAT4X4 mat)
    {
        std::stringstream ss;
        for (size_t i = 0; i < 4; i++)
        {
            for (size_t j = 0; j < 4; j++)
            {
                ss << mat(i, j) << ",";
            }
            ss << "\n";
        }
        return ss.str();
    }
    
    // Stop the sensor loop and release buffer space.
    // Sensor object should be released at the end of the loop function
    void HL2ResearchMode::StopAllSensorDevice()
    {
        m_depthSensorLoopStarted = false;
        //m_pDepthUpdateThread->join();
        if (m_depthMap) 
        {
            delete[] m_depthMap;
            m_depthMap = nullptr;
        }
        if (m_depthMapTexture) 
        {
            delete[] m_depthMapTexture;
            m_depthMapTexture = nullptr;
        }
        if (m_pointCloud) 
        {
            m_pointcloudLength = 0;
            delete[] m_pointCloud;
            m_pointCloud = nullptr;
        }

        m_longDepthSensorLoopStarted = false;
        if (m_longDepthMap)
        {
            delete[] m_longDepthMap;
            m_longDepthMap = nullptr;
        }
        if (m_longDepthMapTexture)
        {
            delete[] m_longDepthMapTexture;
            m_longDepthMapTexture = nullptr;
        }

		m_pSensorDevice->Release();
		m_pSensorDevice = nullptr;
		m_pSensorDeviceConsent->Release();
		m_pSensorDeviceConsent = nullptr;
    }

    com_array<uint16_t> HL2ResearchMode::GetDepthMapBuffer()
    {
        std::lock_guard<std::mutex> l(mu);
        if (!m_depthMap)
        {
            return com_array<uint16_t>();
        }
        com_array<UINT16> tempBuffer = com_array<UINT16>(m_depthMap, m_depthMap + m_depthBufferSize);
        
        return tempBuffer;
    }

    com_array<uint16_t> HL2ResearchMode::GetShortAbImageBuffer()
    {
        std::lock_guard<std::mutex> l(mu);
        if (!m_shortAbImage)
        {
            return com_array<uint16_t>();
        }
        com_array<UINT16> tempBuffer = com_array<UINT16>(m_shortAbImage, m_shortAbImage + m_depthBufferSize);

        return tempBuffer;
    }

    // Get depth map texture buffer. (For visualization purpose)
    com_array<uint8_t> HL2ResearchMode::GetDepthMapTextureBuffer()
    {
        std::lock_guard<std::mutex> l(mu);
        if (!m_depthMapTexture) 
        {
            return com_array<UINT8>();
        }
        com_array<UINT8> tempBuffer = com_array<UINT8>(std::move_iterator(m_depthMapTexture), std::move_iterator(m_depthMapTexture + m_depthBufferSize));

        m_depthMapTextureUpdated = false;
        return tempBuffer;
    }

    // Get depth map texture buffer. (For visualization purpose)
    com_array<uint8_t> HL2ResearchMode::GetShortAbImageTextureBuffer()
    {
        std::lock_guard<std::mutex> l(mu);
        if (!m_shortAbImageTexture)
        {
            return com_array<UINT8>();
        }
        com_array<UINT8> tempBuffer = com_array<UINT8>(std::move_iterator(m_shortAbImageTexture), std::move_iterator(m_shortAbImageTexture + m_depthBufferSize));

        m_shortAbImageTextureUpdated = false;
        return tempBuffer;
    }

    com_array<uint16_t> HL2ResearchMode::GetLongDepthMapBuffer()
    {
        std::lock_guard<std::mutex> l(mu);
        if (!m_longDepthMap)
        {
            return com_array<uint16_t>();
        }
        com_array<UINT16> tempBuffer = com_array<UINT16>(m_longDepthMap, m_longDepthMap + m_longDepthBufferSize);

        return tempBuffer;
    }

    com_array<uint8_t> HL2ResearchMode::GetLongDepthMapTextureBuffer()
    {
        std::lock_guard<std::mutex> l(mu);
        if (!m_longDepthMapTexture)
        {
            return com_array<UINT8>();
        }
        com_array<UINT8> tempBuffer = com_array<UINT8>(std::move_iterator(m_longDepthMapTexture), std::move_iterator(m_longDepthMapTexture + m_longDepthBufferSize));

        m_longDepthMapTextureUpdated = false;
        return tempBuffer;
    }

	com_array<uint8_t> HL2ResearchMode::GetLFCameraBuffer()
	{
		std::lock_guard<std::mutex> l(mu);
		if (!m_LFImage)
		{
			return com_array<UINT8>();
		}
		com_array<UINT8> tempBuffer = com_array<UINT8>(std::move_iterator(m_LFImage), std::move_iterator(m_LFImage + m_LFbufferSize));

		m_LFImageUpdated = false;
		return tempBuffer;
	}

	com_array<uint8_t> HL2ResearchMode::GetRFCameraBuffer()
	{
		std::lock_guard<std::mutex> l(mu);
		if (!m_RFImage)
		{
			return com_array<UINT8>();
		}
		com_array<UINT8> tempBuffer = com_array<UINT8>(std::move_iterator(m_RFImage), std::move_iterator(m_RFImage + m_RFbufferSize));

		m_RFImageUpdated = false;
		return tempBuffer;
	}


    // Get the buffer for point cloud in the form of float array.
    // There will be 3n elements in the array where the 3i, 3i+1, 3i+2 element correspond to x, y, z component of the i'th point. (i->[0,n-1])
    com_array<float> HL2ResearchMode::GetPointCloudBuffer()
    {
        std::lock_guard<std::mutex> l(mu);
        if (m_pointcloudLength == 0)
        {
            return com_array<float>();
        }
        com_array<float> tempBuffer = com_array<float>(std::move_iterator(m_pointCloud), std::move_iterator(m_pointCloud + m_pointcloudLength));
        m_pointCloudUpdated = false;
        return tempBuffer;
    }

    // Get the 3D point (float[3]) of center point in depth map. Can be used to render depth cursor.
    com_array<float> HL2ResearchMode::GetCenterPoint()
    {
        std::lock_guard<std::mutex> l(mu);
        com_array<float> centerPoint = com_array<float>(std::move_iterator(m_centerPoint), std::move_iterator(m_centerPoint + 3));

        return centerPoint;
    }

    com_array<float> HL2ResearchMode::GetDepthSensorPosition()
    {
        std::lock_guard<std::mutex> l(mu);
        com_array<float> depthSensorPos = com_array<float>(std::move_iterator(m_depthSensorPosition), std::move_iterator(m_depthSensorPosition + 3));

        return depthSensorPos;
    }

    // Set the reference coordinate system. Need to be set before the sensor loop starts; otherwise, default coordinate will be used.
    void HL2ResearchMode::SetReferenceCoordinateSystem(winrt::Windows::Perception::Spatial::SpatialCoordinateSystem refCoord)
    {
        m_refFrame = refCoord;
    }

    void HL2ResearchMode::SetPointCloudRoiInSpace(float centerX, float centerY, float centerZ, float boundX, float boundY, float boundZ)
    {
        std::lock_guard<std::mutex> l(mu);

        m_useRoiFilter = true;
        m_roiCenter[0] = centerX;
        m_roiCenter[1] = centerY;
        m_roiCenter[2] = -centerZ;

        m_roiBound[0] = boundX;
        m_roiBound[1] = boundY;
        m_roiBound[2] = boundZ;
    }

    void HL2ResearchMode::SetPointCloudDepthOffset(uint16_t offset)
    {
        m_depthOffset = offset;
    }

    long long HL2ResearchMode::checkAndConvertUnsigned(UINT64 val)
    {
        assert(val <= kMaxLongLong);
        return static_cast<long long>(val);
    }

}
