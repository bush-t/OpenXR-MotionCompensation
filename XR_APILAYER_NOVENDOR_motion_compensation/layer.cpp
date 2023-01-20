// MIT License
//
// Copyright(c) 2022 Matthieu Bucchianeri, Sebastian Veith
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this softwareand associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright noticeand this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "pch.h"

#include "layer.h"
#include "tracker.h"
#include "feedback.h"
#include "utility.h"
#include "config.h"
#include <log.h>
#include <util.h>


using namespace motion_compensation_layer::log;
using namespace Feedback;
using namespace xr::math;
namespace motion_compensation_layer
{
    OpenXrLayer::~OpenXrLayer()
    {
        delete m_Tracker;
    }

    XrResult OpenXrLayer::xrDestroyInstance(XrInstance instance)
    {
        if (m_Enabled)
        {
            Log("xrDestroyInstance\n");
        }
        return OpenXrApi::xrDestroyInstance(instance);
    }

    XrResult OpenXrLayer::xrCreateInstance(const XrInstanceCreateInfo* createInfo)
    {
        Log("xrCreateInstance\n");
        if (createInfo->type != XR_TYPE_INSTANCE_CREATE_INFO)
        {
            return XR_ERROR_VALIDATION_FAILURE;
        }

        TraceLoggingWrite(g_traceProvider,
                          "xrCreateInstance",
                          TLArg(xr::ToString(createInfo->applicationInfo.apiVersion).c_str(), "ApiVersion"),
                          TLArg(createInfo->applicationInfo.applicationName, "ApplicationName"),
                          TLArg(createInfo->applicationInfo.applicationVersion, "ApplicationVersion"),
                          TLArg(createInfo->applicationInfo.engineName, "EngineName"),
                          TLArg(createInfo->applicationInfo.engineVersion, "EngineVersion"),
                          TLArg(createInfo->createFlags, "CreateFlags"));

        for (uint32_t i = 0; i < createInfo->enabledApiLayerCount; i++)
        {
            TraceLoggingWrite(g_traceProvider,
                              "xrCreateInstance",
                              TLArg(createInfo->enabledApiLayerNames[i], "ApiLayerName"));
        }
        for (uint32_t i = 0; i < createInfo->enabledExtensionCount; i++)
        {
            TraceLoggingWrite(g_traceProvider,
                              "xrCreateInstance",
                              TLArg(createInfo->enabledExtensionNames[i], "ExtensionName"));
        }

        // Needed to resolve the requested function pointers.
        const XrResult result = OpenXrApi::xrCreateInstance(createInfo);

        m_Application = GetApplicationName();

        // Dump the application name and OpenXR runtime information to help debugging issues.
        XrInstanceProperties instanceProperties = {XR_TYPE_INSTANCE_PROPERTIES};
        CHECK_XRCMD(OpenXrApi::xrGetInstanceProperties(GetXrInstance(), &instanceProperties));
        m_RuntimeName = fmt::format("{} {}.{}.{}",
                                             instanceProperties.runtimeName,
                                             XR_VERSION_MAJOR(instanceProperties.runtimeVersion),
                                             XR_VERSION_MINOR(instanceProperties.runtimeVersion),
                                             XR_VERSION_PATCH(instanceProperties.runtimeVersion));
        TraceLoggingWrite(g_traceProvider, "xrCreateInstance", TLArg(m_RuntimeName.c_str(), "RuntimeName"));
        TraceLoggingWrite(g_traceProvider, "xrCreateInstance", TLArg(m_Application.c_str(), "ApplicationName"));
        Log("Application: %s\n", m_Application.c_str());
        Log("Using OpenXR runtime: %s\n", m_RuntimeName.c_str());

        // Initialize Configuration
        m_Initialized = GetConfig()->Init(m_Application);

        if (m_Initialized)
        {
            GetConfig()->GetBool(Cfg::Enabled, m_Enabled);
            if (!m_Enabled)
            {
                Log("motion compensation disabled in config file\n");
                return result;
            }

            // enable / disable physical tracker initialization
            GetConfig()->GetBool(Cfg::PhysicalEnabled, m_PhysicalEnabled);
            if (!m_PhysicalEnabled)
            {
                Log("initialization of physical tracker disabled in config file\n");
            }

            // initialize audio feedback
            GetAudioOut()->Init();

            // enable debug test rotation
            GetConfig()->GetBool(Cfg::TestRotation, m_TestRotation);

            // choose cache for reverting pose in xrEndFrame
            GetConfig()->GetBool(Cfg::CacheUseEye, m_UseEyeCache);

            float timeout;
            if (GetConfig()->GetFloat(Cfg::TrackerTimeout, timeout))
            {
                m_RecoveryWait = static_cast<XrTime>(timeout * 1000000000.0);
                Log("tracker timeout is set to %.3f ms\n", m_RecoveryWait / 1000000.0);
            }
            else
            {
                ErrorLog("%s: defaulting to tracker timeout of %.3f ms\n", __FUNCTION__,  m_RecoveryWait / 1000000.0);
            }

            float cacheTolerance{2.0};
            GetConfig()->GetFloat(Cfg::CacheTolerance, cacheTolerance);
            Log("cache tolerance is set to %.3f ms\n", cacheTolerance);
            const auto toleranceTime = static_cast<XrTime>(cacheTolerance * 1000000.0);
            m_PoseCache.SetTolerance(toleranceTime);
            m_EyeCache.SetTolerance(toleranceTime);
        }

        // initialize tracker
        Tracker::GetTracker(&m_Tracker);
        if (!m_Tracker->Init() || !m_ViveTracker.Init())
        {
            m_Initialized = false;
        }

        // initialize keyboard input handler
        m_Input = std::make_unique<Input::InputHandler>(Input::InputHandler(this));
        if (!m_Input->Init())
        {
            m_Initialized = false;
        }

        CreateTrackerAction();

        return result;
    }

    XrResult OpenXrLayer::xrGetSystem(XrInstance instance, const XrSystemGetInfo* getInfo, XrSystemId* systemId)
    {
        if (!m_Enabled)
        {
            return OpenXrApi::xrGetSystem(instance, getInfo, systemId);
        }

        DebugLog("xrGetSystem\n");
        if (getInfo->type != XR_TYPE_SYSTEM_GET_INFO)
        {
            return XR_ERROR_VALIDATION_FAILURE;
        }

        TraceLoggingWrite(g_traceProvider,
                          "xrGetSystem",
                          TLPArg(instance, "Instance"),
                          TLArg(xr::ToCString(getInfo->formFactor), "FormFactor"));

        const XrResult result = OpenXrApi::xrGetSystem(instance, getInfo, systemId);
        if (XR_SUCCEEDED(result) && getInfo->formFactor == XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY)
        {
            if (*systemId != m_systemId)
            {
                XrSystemProperties systemProperties{XR_TYPE_SYSTEM_PROPERTIES};
                CHECK_XRCMD(OpenXrApi::xrGetSystemProperties(instance, *systemId, &systemProperties));
                TraceLoggingWrite(g_traceProvider, "xrGetSystem", TLArg(systemProperties.systemName, "SystemName"));
                Log("Using OpenXR system: %s\n", systemProperties.systemName);
            }

            // Remember the XrSystemId to use.
            m_systemId = *systemId;
        }

        TraceLoggingWrite(g_traceProvider, "xrGetSystem", TLArg((int)*systemId, "SystemId"));

        return result;
    }

    XrResult OpenXrLayer::xrCreateSession(XrInstance instance,
                                          const XrSessionCreateInfo* createInfo,
                                          XrSession* session)
    {
        if (!m_Enabled)
        {
            return OpenXrApi::xrCreateSession(instance, createInfo, session);
        }

        Log("xrCreateSession\n");
        if (createInfo->type != XR_TYPE_SESSION_CREATE_INFO)
        {
            return XR_ERROR_VALIDATION_FAILURE;
        }

        TraceLoggingWrite(g_traceProvider,
                          "xrCreateSession",
                          TLPArg(instance, "Instance"),
                          TLArg((int)createInfo->systemId, "SystemId"),
                          TLArg(createInfo->createFlags, "CreateFlags"));
        const XrResult result = OpenXrApi::xrCreateSession(instance, createInfo, session);
        if (XR_SUCCEEDED(result))
        {
            if (isSystemHandled(createInfo->systemId))
            {
                // enable / disable graphical overlay initialization
                GetConfig()->GetBool(Cfg::OverlayEnabled, m_OverlayEnabled);
                if (m_OverlayEnabled)
                {
                    m_Overlay = std::make_unique<graphics::Overlay>(graphics::Overlay());
                    m_Overlay->CreateSession(createInfo, session, m_RuntimeName);
                }
                else
                {
                    Log("initialization of graphical overlay disabled in config file\n");
                }
                
                m_Session = *session;

                CreateTrackerActionSpace();

                const XrReferenceSpaceCreateInfo referenceSpaceCreateInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO,
                                                                          nullptr,
                                                                          XR_REFERENCE_SPACE_TYPE_VIEW,
                                                                          Pose::Identity()};
                CHECK_XRCMD(xrCreateReferenceSpace(*session, &referenceSpaceCreateInfo, &m_ViewSpace));
            }

            TraceLoggingWrite(g_traceProvider, "xrCreateSession", TLPArg(*session, "Session"));
        }

        return result;
    }

    XrResult OpenXrLayer::xrBeginSession(XrSession session, const XrSessionBeginInfo* beginInfo)
    {
        if (!m_Enabled)
        {
            return OpenXrApi::xrBeginSession(session, beginInfo);
        }

        Log("xrBeginSession\n");
        if (beginInfo->type != XR_TYPE_SESSION_BEGIN_INFO)
        {
            return XR_ERROR_VALIDATION_FAILURE;
        }
        
        TraceLoggingWrite(
            g_traceProvider,
            "xrBeginSession",
            TLPArg(session, "Session"),
            TLArg(xr::ToCString(beginInfo->primaryViewConfigurationType), "PrimaryViewConfigurationType"));

        const XrResult result = OpenXrApi::xrBeginSession(session, beginInfo);
        m_ViewConfigType = beginInfo->primaryViewConfigurationType;

        return result;
    }

    XrResult OpenXrLayer::xrEndSession(XrSession session)
    {
        if (m_Enabled)
        {
            Log("xrEndSession\n");
            TraceLoggingWrite(g_traceProvider, "xrEndssion", TLPArg(session, "Session"));
        }
        return OpenXrApi::xrEndSession(session);
    }

    XrResult OpenXrLayer::xrDestroySession(XrSession session)
    {
        if (m_Enabled)
        {
            if (XR_NULL_HANDLE != m_TrackerSpace)
            {
                GetInstance()->xrDestroySpace(m_TrackerSpace);
                m_TrackerSpace = XR_NULL_HANDLE;
            }
            Log("xrDestroySession\n");
            TraceLoggingWrite(g_traceProvider, "xrDestroySession", TLPArg(session, "Session"));
        }
        const XrResult result = OpenXrApi::xrDestroySession(session);

        if (m_Enabled && m_OverlayEnabled)
        {
            m_Overlay->DestroySession();
        }
        m_Overlay.reset();

        return result;
    }

    XrResult OpenXrLayer::xrCreateSwapchain(XrSession session,
                               const XrSwapchainCreateInfo* createInfo,
                               XrSwapchain* swapchain)
    {
        if (!m_Enabled || !m_OverlayEnabled)
        {
            return OpenXrApi::xrCreateSwapchain(session, createInfo, swapchain);
        }

        DebugLog("xrCreateSwapchain\n");
        if (createInfo->type != XR_TYPE_SWAPCHAIN_CREATE_INFO)
        {
            return XR_ERROR_VALIDATION_FAILURE;
        }

        TraceLoggingWrite(g_traceProvider,
                          "xrCreateSwapchain",
                          TLPArg(session, "Session"),
                          TLArg(createInfo->arraySize, "ArraySize"),
                          TLArg(createInfo->width, "Width"),
                          TLArg(createInfo->height, "Height"),
                          TLArg(createInfo->createFlags, "CreateFlags"),
                          TLArg(createInfo->format, "Format"),
                          TLArg(createInfo->faceCount, "FaceCount"),
                          TLArg(createInfo->mipCount, "MipCount"),
                          TLArg(createInfo->sampleCount, "SampleCount"),
                          TLArg(createInfo->usageFlags, "UsageFlags"));

        if (!isSessionHandled(session) || !m_Overlay->m_Initialized)
        {
            return OpenXrApi::xrCreateSwapchain(session, createInfo, swapchain);
        }
        Log("Creating swapchain with dimensions=%ux%u, arraySize=%u, mipCount=%u, sampleCount=%u, format=%d, "
            "usage=0x%x\n",
            createInfo->width,
            createInfo->height,
            createInfo->arraySize,
            createInfo->mipCount,
            createInfo->sampleCount,
            createInfo->format,
            createInfo->usageFlags);

        const XrResult result = OpenXrApi::xrCreateSwapchain(session, createInfo, swapchain);
        if (XR_SUCCEEDED(result))
        {
            m_Overlay->CreateSwapchain(session, createInfo, swapchain);
        }
        return result;
    }

    XrResult OpenXrLayer::xrDestroySwapchain(XrSwapchain swapchain)
    {
        if (!m_Enabled || !m_OverlayEnabled)
        {
            return OpenXrApi::xrDestroySwapchain(swapchain);
        }

        DebugLog("xrDestroySwapchain\n");
        TraceLoggingWrite(g_traceProvider, "xrDestroySwapchain", TLPArg(swapchain, "Swapchain"));

        const XrResult result = OpenXrApi::xrDestroySwapchain(swapchain);
        if (XR_SUCCEEDED(result))
        {
            m_Overlay->DestroySwapchain(swapchain);
        }

        return result;
    }

    XrResult OpenXrLayer::xrWaitSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageWaitInfo* waitInfo)
    {
        if (!m_Enabled || !m_OverlayEnabled)
        {
            return OpenXrApi::xrWaitSwapchainImage(swapchain, waitInfo);
        }

        DebugLog("xrWaitSwapchainImage\n");
        if (waitInfo->type != XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO)
        {
            return XR_ERROR_VALIDATION_FAILURE;
        }

        TraceLoggingWrite(g_traceProvider,
                          "xrWaitSwapchainImage",
                          TLPArg(swapchain, "Swapchain"),
                          TLArg(waitInfo->timeout));

        // We remove the timeout causing issues with OpenComposite.
        XrSwapchainImageWaitInfo chainWaitInfo = *waitInfo;
        if (m_Enabled)
        {
            chainWaitInfo.timeout = XR_INFINITE_DURATION;
        }
        return OpenXrApi::xrWaitSwapchainImage(swapchain, &chainWaitInfo);
    }

    XrResult OpenXrLayer::xrAcquireSwapchainImage(XrSwapchain swapchain,
                                     const XrSwapchainImageAcquireInfo* acquireInfo,
                                     uint32_t* index)
    {
        if (!m_Enabled || !m_OverlayEnabled)
        {
            return OpenXrApi::xrAcquireSwapchainImage(swapchain, acquireInfo, index);
        }

        DebugLog("xrAcquireSwapchainImage\n");
        if (acquireInfo && acquireInfo->type != XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO)
        {
            return XR_ERROR_VALIDATION_FAILURE;
        }

        TraceLoggingWrite(g_traceProvider, "xrAcquireSwapchainImage", TLPArg(swapchain, "Swapchain"));

        if (!m_Overlay->m_Initialized)
        {
            return OpenXrApi::xrAcquireSwapchainImage(swapchain, acquireInfo, index);
        }

        return m_Overlay->AcquireSwapchainImage(swapchain, acquireInfo, index);
    }

    XrResult OpenXrLayer::xrReleaseSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageReleaseInfo* releaseInfo)
    {
        if (!m_Enabled || !m_OverlayEnabled)
        {
            return OpenXrApi::xrReleaseSwapchainImage(swapchain, releaseInfo);
        }

        DebugLog("xrReleaseSwapchainImage\n");
        if (releaseInfo && releaseInfo->type != XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO)
        {
            return XR_ERROR_VALIDATION_FAILURE;
        }

        TraceLoggingWrite(g_traceProvider, "xrReleaseSwapchainImage", TLPArg(swapchain, "Swapchain"));

        if (!m_Overlay->m_Initialized)
        {
            return OpenXrApi::xrReleaseSwapchainImage(swapchain, releaseInfo);
        }
        return m_Overlay->ReleaseSwapchainImage(swapchain, releaseInfo);
    }

    XrResult OpenXrLayer::xrGetCurrentInteractionProfile(XrSession session,
                                                         XrPath topLevelUserPath,
                                                         XrInteractionProfileState* interactionProfile)
    {
        const XrResult result =  OpenXrApi::xrGetCurrentInteractionProfile(session, topLevelUserPath, interactionProfile);
        if (m_Enabled && XR_SUCCEEDED(result) && interactionProfile)
        {
            Log("current interaction profile for %s: %s\n",
                getXrPath(topLevelUserPath).c_str(),
                XR_NULL_PATH != interactionProfile->interactionProfile
                    ? getXrPath(interactionProfile->interactionProfile).c_str()
                    : "XR_NULL_PATH");
        }
        return result;
    }

    XrResult
    OpenXrLayer::xrSuggestInteractionProfileBindings(XrInstance instance,
                                                     const XrInteractionProfileSuggestedBinding* suggestedBindings)
    {
        if (!m_Enabled || !m_PhysicalEnabled)
        {
            return OpenXrApi::xrSuggestInteractionProfileBindings(instance, suggestedBindings);
        }

        if (suggestedBindings->type != XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING)
        {
            return XR_ERROR_VALIDATION_FAILURE;
        }

        const std::string profile = getXrPath(suggestedBindings->interactionProfile);
        Log("xrSuggestInteractionProfileBindings: %s\n", profile.c_str());
        TraceLoggingWrite(g_traceProvider,
                          "xrSuggestInteractionProfileBindings",
                          TLPArg(instance, "Instance"),
                          TLArg(profile.c_str(), "InteractionProfile"));

        for (uint32_t i = 0; i < suggestedBindings->countSuggestedBindings; i++)
        {
            TraceLoggingWrite(g_traceProvider,
                              "xrSuggestInteractionProfileBindings",
                              TLPArg(suggestedBindings->suggestedBindings[i].action, "Action"),
                              TLArg(getXrPath(suggestedBindings->suggestedBindings[i].binding).c_str(), "Path"));

            DebugLog("binding: %s\n", getXrPath(suggestedBindings->suggestedBindings[i].binding).c_str());
        }

        if (m_ActionSetAttached)
        {
            // detach and recreate actionset and trackerspace
            if (XR_NULL_HANDLE != m_ActionSet)
            {
                GetInstance()->xrDestroyActionSet(m_ActionSet);
            }
            if (XR_NULL_HANDLE != m_TrackerSpace)
            {
                GetInstance()->xrDestroySpace(m_TrackerSpace);
                m_TrackerSpace = XR_NULL_HANDLE;
            }
            CreateTrackerAction();
            CreateTrackerActionSpace();
            m_ActionSetAttached = false;
            m_InteractionProfileSuggested = false;
            Log("detached and recreated tracker action\n");
        }

        XrInteractionProfileSuggestedBinding bindingProfiles = *suggestedBindings;
        std::vector<XrActionSuggestedBinding> bindings{};

        bindings.resize((uint32_t)bindingProfiles.countSuggestedBindings);
        memcpy(bindings.data(),
               bindingProfiles.suggestedBindings,
               bindingProfiles.countSuggestedBindings * sizeof(XrActionSuggestedBinding));

        const std::string trackerInput(m_ViveTracker.active
                                           ? m_ViveTracker.role + "input"
                                           : "/user/hand/" + GetConfig()->GetControllerSide() + "/input");
        const std::string path(trackerInput + "/grip/pose");
        bool isTrackerInput = false;
        bool bindingOverriden = false;
        for (XrActionSuggestedBinding& curBinding : bindings)
        {
            // find and override tracker pose action
            const std::string bindingPath = getXrPath(curBinding.binding);
            if (!bindingPath.compare(0, trackerInput.size(), trackerInput))
            {
                // path starts with user/hand/<side>/input
                isTrackerInput = true;

                if (getXrPath(curBinding.binding) == path)
                {
                    // TODO: find a way to persist original pose action?
                    curBinding.action = m_TrackerPoseAction;
                    bindingOverriden = true;
                    m_InteractionProfileSuggested = true;
                    Log("Binding %s - %s overridden with reference tracker action\n", profile.c_str(), path.c_str());
                }
            }
        }
        if (isTrackerInput && !bindingOverriden)
        {
            // suggestion is for tracker input but doesn't include pose -> add it
            XrActionSuggestedBinding newBinding{m_TrackerPoseAction};
            CHECK_XRCMD(xrStringToPath(GetXrInstance(), path.c_str(), &newBinding.binding));
            bindings.push_back(newBinding);
            m_InteractionProfileSuggested = true;
            Log("Binding %s - %s for tracker action added\n", profile.c_str(), path.c_str());
        }

        bindingProfiles.suggestedBindings = bindings.data();
        bindingProfiles.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
        return OpenXrApi::xrSuggestInteractionProfileBindings(instance, &bindingProfiles);
    }

    XrResult OpenXrLayer::xrAttachSessionActionSets(XrSession session, const XrSessionActionSetsAttachInfo* attachInfo)
    {
        if (!m_Enabled || !m_PhysicalEnabled)
        {
            return OpenXrApi::xrAttachSessionActionSets(session, attachInfo);
        }

        Log("xrAttachSessionActionSets\n");
        if (attachInfo->type != XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO)
        {
            return XR_ERROR_VALIDATION_FAILURE;
        }

        TraceLoggingWrite(g_traceProvider, "xrAttachSessionActionSets", TLPArg(session, "Session"));
        for (uint32_t i = 0; i < attachInfo->countActionSets; i++)
        {
            TraceLoggingWrite(g_traceProvider,
                              "xrAttachSessionActionSets",
                              TLPArg(attachInfo->actionSets[i], "ActionSet"));
        }

        if (!m_InteractionProfileSuggested)
        {
            // suggest fallback in case application does not suggest any bindings
            XrInteractionProfileSuggestedBinding suggestedBindings{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING,
                                                                   nullptr};
            XrActionSuggestedBinding binding{m_TrackerPoseAction};

            const std::string profile{m_ViveTracker.active ? m_ViveTracker.profile
                                                           : "/interaction_profiles/khr/simple_controller"};
            CHECK_XRCMD(xrStringToPath(GetXrInstance(), profile.c_str(), &suggestedBindings.interactionProfile));

            const std::string path{
                (m_ViveTracker.active ? m_ViveTracker.role : "/user/hand/" + GetConfig()->GetControllerSide()) +
                "/input/grip/pose"};
            CHECK_XRCMD(xrStringToPath(GetXrInstance(), path.c_str(), &binding.binding));

            suggestedBindings.suggestedBindings = &binding;
            suggestedBindings.countSuggestedBindings = 1;
            CHECK_XRCMD(OpenXrApi::xrSuggestInteractionProfileBindings(GetXrInstance(), &suggestedBindings));
            m_InteractionProfileSuggested = true;
            Log("suggested %s before action set attachment\n", profile.c_str());
            TraceLoggingWrite(g_traceProvider,
                              "OpenXrTracker::xrAttachSessionActionSets",
                              TLArg(profile.c_str(), "Profile"),
                              TLPArg(binding.action, "Action"),
                              TLArg(path.c_str(), "Path"));
        }

        XrSessionActionSetsAttachInfo chainAttachInfo = *attachInfo;
        std::vector<XrActionSet> newActionSets;

        newActionSets.resize((size_t)chainAttachInfo.countActionSets);
        memcpy(newActionSets.data(), chainAttachInfo.actionSets, chainAttachInfo.countActionSets * sizeof(XrActionSet));
        newActionSets.push_back(m_ActionSet);

        chainAttachInfo.actionSets = newActionSets.data();
        chainAttachInfo.countActionSets = static_cast<uint32_t>(newActionSets.size());

        const XrResult result = OpenXrApi::xrAttachSessionActionSets(session, &chainAttachInfo);
        if (XR_SUCCEEDED(result))
        {
            Log("tracker action set attached\n");
            m_ActionSetAttached = true;
        }
        return result;
    }

    XrResult OpenXrLayer::xrCreateReferenceSpace(XrSession session,
                                                 const XrReferenceSpaceCreateInfo* createInfo,
                                                 XrSpace* space)
    {
        if (!m_Enabled)
        {
            return OpenXrApi::xrCreateReferenceSpace(session, createInfo, space);
        }

        if (createInfo->type != XR_TYPE_REFERENCE_SPACE_CREATE_INFO)
        {
            return XR_ERROR_VALIDATION_FAILURE;
        }

        DebugLog("xrCreateReferenceSpace: %u type: %d \n", *space, createInfo->referenceSpaceType);
        TraceLoggingWrite(g_traceProvider,
                          "xrCreateReferenceSpace",
                          TLPArg(session, "Session"),
                          TLArg(xr::ToCString(createInfo->referenceSpaceType), "ReferenceSpaceType"),
                          TLArg(xr::ToString(createInfo->poseInReferenceSpace).c_str(), "PoseInReferenceSpace"));

        const XrResult result = OpenXrApi::xrCreateReferenceSpace(session, createInfo, space);
        if (XR_SUCCEEDED(result))
        {
            if (XR_REFERENCE_SPACE_TYPE_VIEW == createInfo->referenceSpaceType)
            {
                Log("creation of view space detected: %u\n", *space);
                DebugLog("view pose: %s\n", xr::ToString(createInfo->poseInReferenceSpace).c_str());

                // memorize view spaces
                TraceLoggingWrite(g_traceProvider, "xrCreateReferenceSpace", TLArg("View_Space", "Added"));
                m_ViewSpaces.insert(*space);
            }
            else if (XR_REFERENCE_SPACE_TYPE_LOCAL == createInfo->referenceSpaceType)
            {
                Log("creation of local reference space detected: %u\n", *space);
                DebugLog("local pose: %s\n", xr::ToString(createInfo->poseInReferenceSpace).c_str());
                
                // disable mc temporarily until series of reference space creations is over
                m_RecenterInProgress = true;
                m_LocalRefSpaceCreated = true;

                if (m_Tracker->m_Calibrated)
                {
                    // adjust reference pose to match newly created ref space
                    XrSpaceLocation location{XR_TYPE_SPACE_LOCATION, nullptr};
                    if (XR_SUCCEEDED(xrLocateSpace(m_ReferenceSpace, *space, m_LastFrameTime, &location)))
                    {
                        DebugLog("old space to new space: %s\n", xr::ToString(location.pose).c_str());
                        m_Tracker->AdjustReferencePose(location.pose);
                    }
                    else
                    {
                        ErrorLog("unable to adjust reference pose to newly created reference space");
                    }
                }
                m_ReferenceSpace = *space;
            }
        }
        TraceLoggingWrite(g_traceProvider, "xrCreateReferenceSpace", TLPArg(*space, "Space"));

        return result;
    }

    XrResult OpenXrLayer::xrLocateSpace(XrSpace space, XrSpace baseSpace, XrTime time, XrSpaceLocation* location)
    {
        if (!m_Enabled)
        {
            return OpenXrApi::xrLocateSpace(space, baseSpace, time, location);
        }

        if (location->type != XR_TYPE_SPACE_LOCATION)
        {
            return XR_ERROR_VALIDATION_FAILURE;
        }

        DebugLog("xrLocateSpace(%u): %u %u\n", time, space, baseSpace);
        TraceLoggingWrite(g_traceProvider,
                          "xrLocateSpace",
                          TLPArg(space, "Space"),
                          TLPArg(baseSpace, "BaseSpace"),
                          TLArg(time, "Time"));

        // determine original location
        CHECK_XRCMD(OpenXrApi::xrLocateSpace(space, baseSpace, time, location));

        if (m_Activated && !m_RecenterInProgress && (isViewSpace(space) || isViewSpace(baseSpace)))
        {
            TraceLoggingWrite(g_traceProvider,
                              "xrLocateSpace",
                              TLArg(xr::ToString(location->pose).c_str(), "PoseBefore"),
                              TLArg(location->locationFlags, "LocationFlags"));

            // manipulate pose using tracker
            const bool spaceIsViewSpace = isViewSpace(space);
            const bool baseSpaceIsViewSpace = isViewSpace(baseSpace);

            XrPosef trackerDelta = Pose::Identity();
            if (!m_TestRotation ? m_Tracker->GetPoseDelta(trackerDelta, m_Session, time)
                                : TestRotation(&trackerDelta, time, false))
            {
                m_RecoveryStart = 0;
                if (spaceIsViewSpace && !baseSpaceIsViewSpace)
                {
                    location->pose = Pose::Multiply(location->pose, trackerDelta);
                }
                if (baseSpaceIsViewSpace && !spaceIsViewSpace)
                {
                    // TODO: verify calculation
                    location->pose = Pose::Multiply(location->pose, Pose::Invert(trackerDelta));
                }
            }
            else
            {
                if (0 == m_RecoveryStart)
                {
                    ErrorLog("unable to retrieve tracker pose delta\n");
                    m_RecoveryStart = time;
                }
                else if (m_RecoveryWait >= 0 && time - m_RecoveryStart > m_RecoveryWait)
                {
                    ErrorLog("tracker connection lost\n");
                    GetAudioOut()->Execute(Event::ConnectionLost);
                    m_Activated = false;  
                    m_RecoveryStart = -1;
                }
            }

            // safe pose for use in xrEndFrame
            m_PoseCache.AddSample(time, trackerDelta);

            TraceLoggingWrite(g_traceProvider,
                              "xrLocateSpace",
                              TLArg(xr::ToString(location->pose).c_str(), "PoseAfter"));
        }

        return XR_SUCCESS;
    }

    XrResult OpenXrLayer::xrLocateViews(XrSession session,
                                        const XrViewLocateInfo* viewLocateInfo,
                                        XrViewState* viewState,
                                        uint32_t viewCapacityInput,
                                        uint32_t* viewCountOutput,
                                        XrView* views)
    {
        if (!m_Enabled)
        {
            return OpenXrApi::xrLocateViews(session,
                                            viewLocateInfo,
                                            viewState,
                                            viewCapacityInput,
                                            viewCountOutput,
                                            views);
        }

        if (viewLocateInfo->type != XR_TYPE_VIEW_LOCATE_INFO)
        {
            return XR_ERROR_VALIDATION_FAILURE;
        }

        DebugLog("xrLocateViews(%u): %u\n", viewLocateInfo->displayTime, viewLocateInfo->space);
        TraceLoggingWrite(g_traceProvider,
                          "xrLocateViews",
                          TLPArg(session, "Session"),
                          TLArg(xr::ToCString(viewLocateInfo->viewConfigurationType), "ViewConfigurationType"),
                          TLArg(viewLocateInfo->displayTime, "DisplayTime"),
                          TLPArg(viewLocateInfo->space, "Space"),
                          TLArg(viewCapacityInput, "ViewCapacityInput"));

        CHECK_XRCMD(
            OpenXrApi::xrLocateViews(session, viewLocateInfo, viewState, viewCapacityInput, viewCountOutput, views));

        TraceLoggingWrite(g_traceProvider, "xrLocateViews", TLArg(viewState->viewStateFlags, "ViewStateFlags"));

        if (!m_Activated)
        {
            return XR_SUCCESS;
        }

        // store eye poses to avoid recalculation in xrEndFrame?
        std::vector<XrPosef> originalEyePoses{};
        for (uint32_t i = 0; i < *viewCountOutput; i++)
        {
            originalEyePoses.push_back(views[i].pose);
        }
        m_EyeCache.AddSample(viewLocateInfo->displayTime, originalEyePoses);

        if (m_EyeOffsets.empty())
        {
            // determine eye poses
            const XrViewLocateInfo offsetViewLocateInfo{viewLocateInfo->type,
                                                        nullptr,
                                                        viewLocateInfo->viewConfigurationType,
                                                        viewLocateInfo->displayTime,
                                                        m_ViewSpace};

            CHECK_XRCMD(OpenXrApi::xrLocateViews(session,
                                                 &offsetViewLocateInfo,
                                                 viewState,
                                                 viewCapacityInput,
                                                 viewCountOutput,
                                                 views));
            for (uint32_t i = 0; i < *viewCountOutput; i++)
            {
                m_EyeOffsets.push_back(views[i]);
            }
        }

        // manipulate reference space location
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION, nullptr};
        CHECK_XRCMD(xrLocateSpace(m_ViewSpace, viewLocateInfo->space, viewLocateInfo->displayTime, &location));
        for (uint32_t i = 0; i < *viewCountOutput; i++)
        {
            TraceLoggingWrite(g_traceProvider, "xrLocateViews", TLArg(xr::ToString(views[i].fov).c_str(), "Fov"));
            TraceLoggingWrite(g_traceProvider,
                              "xrLocateViews",
                              TLArg(i, "Index"),
                              TLArg(xr::ToString(views[i].pose).c_str(), "PoseBefore"));

            // apply manipulation
            views[i].pose = Pose::Multiply(m_EyeOffsets[i].pose, location.pose);

            TraceLoggingWrite(g_traceProvider,
                              "xrLocateViews",
                              TLArg(i, "Index"),
                              TLArg(xr::ToString(views[i].pose).c_str(), "PoseAfter"));
        }

        return XR_SUCCESS;
    }

    XrResult OpenXrLayer::xrSyncActions(XrSession session, const XrActionsSyncInfo* syncInfo)
    {
        if (!m_Enabled || !m_PhysicalEnabled)
        {
            return OpenXrApi::xrSyncActions(session, syncInfo);
        }

        DebugLog("xrSyncActions\n");
        if (syncInfo->type != XR_TYPE_ACTIONS_SYNC_INFO)
        {
            return XR_ERROR_VALIDATION_FAILURE;
        }
        
        TraceLoggingWrite(g_traceProvider, "xrSyncActions", TLPArg(session, "Session"));
        for (uint32_t i = 0; i < syncInfo->countActiveActionSets; i++)
        {
            TraceLoggingWrite(g_traceProvider,
                              "xrSyncActions",
                              TLPArg(syncInfo->activeActionSets[i].actionSet, "ActionSet"),
                              TLArg(syncInfo->activeActionSets[i].subactionPath, "SubactionPath"));
        }

        XrActionsSyncInfo chainSyncInfo = *syncInfo;
        std::vector<XrActiveActionSet> newActiveActionSets;
        const auto trackerActionSet = m_ActionSet;
        if (trackerActionSet != XR_NULL_HANDLE)
        {
            newActiveActionSets.resize(static_cast<size_t>(chainSyncInfo.countActiveActionSets) + 1);
            memcpy(newActiveActionSets.data(),
                   chainSyncInfo.activeActionSets,
                   chainSyncInfo.countActiveActionSets * sizeof(XrActionSet));
            uint32_t nextActionSetSlot = chainSyncInfo.countActiveActionSets;

            newActiveActionSets[nextActionSetSlot].actionSet = trackerActionSet;
            newActiveActionSets[nextActionSetSlot++].subactionPath = XR_NULL_PATH;

            chainSyncInfo.activeActionSets = newActiveActionSets.data();
            chainSyncInfo.countActiveActionSets = nextActionSetSlot;
        }

        return OpenXrApi::xrSyncActions(session, &chainSyncInfo);
    }

    XrResult OpenXrLayer::xrBeginFrame(XrSession session, const XrFrameBeginInfo* frameBeginInfo)
    {
        if (!m_Enabled || !m_OverlayEnabled)
        {
            return OpenXrApi::xrBeginFrame(session, frameBeginInfo);
        }

        if (frameBeginInfo && frameBeginInfo->type != XR_TYPE_FRAME_BEGIN_INFO)
        {
            return XR_ERROR_VALIDATION_FAILURE;
        }
        DebugLog("xrBeginFrame\n");

        TraceLoggingWrite(g_traceProvider, "xrBeginFrame", TLPArg(session, "Session"));

        m_Overlay->BeginFrameBefore();

        const XrResult result = OpenXrApi::xrBeginFrame(session, frameBeginInfo);

        if (XR_SUCCEEDED(result) && isSessionHandled(session))
        {
            m_Overlay->BeginFrameAfter();
        }

        return result;
    }

    XrResult OpenXrLayer::xrEndFrame(XrSession session, const XrFrameEndInfo* frameEndInfo)
    {
        if (!m_Enabled || !isSessionHandled(session))
        {
            return OpenXrApi::xrEndFrame(session, frameEndInfo);
        }

        if (frameEndInfo->type != XR_TYPE_FRAME_END_INFO)
        {
            return XR_ERROR_VALIDATION_FAILURE;
        }

        DebugLog("xrEndframe(%u)\n", frameEndInfo->displayTime);
        TraceLoggingWrite(g_traceProvider,
                          "xrEndFrame",
                          TLPArg(session, "Session"),
                          TLArg(frameEndInfo->displayTime, "DisplayTime"),
                          TLArg(xr::ToCString(frameEndInfo->environmentBlendMode), "EnvironmentBlendMode"));

        m_LastFrameTime = frameEndInfo->displayTime;
        if (m_RecenterInProgress && !m_LocalRefSpaceCreated)
        {
            m_RecenterInProgress = false;
        }
        m_LocalRefSpaceCreated = false; 

        XrFrameEndInfo chainFrameEndInfo = *frameEndInfo;
       
        XrPosef referenceTrackerPose = m_Tracker->GetReferencePose(m_Session, chainFrameEndInfo.displayTime);

        XrPosef reversedManipulation;
        std::vector<XrPosef> cachedEyePoses;
        if (m_Activated)
        {
            reversedManipulation = Pose::Invert(m_PoseCache.GetSample(chainFrameEndInfo.displayTime));
            m_PoseCache.CleanUp(chainFrameEndInfo.displayTime);
            cachedEyePoses =
                m_UseEyeCache ? m_EyeCache.GetSample(chainFrameEndInfo.displayTime) : std::vector<XrPosef>();
            m_EyeCache.CleanUp(chainFrameEndInfo.displayTime);
        }
        if (m_OverlayEnabled)
        {
            m_Overlay->DrawOverlay(&chainFrameEndInfo, referenceTrackerPose, reversedManipulation, m_Activated);
        }

        if (!m_Activated)
        {
            m_Input->HandleKeyboardInput(chainFrameEndInfo.displayTime);
            return OpenXrApi::xrEndFrame(session, &chainFrameEndInfo);
        }

        std::vector<const XrCompositionLayerBaseHeader*> resetLayers{};
        std::vector<XrCompositionLayerProjection*> resetProjectionLayers{};
        std::vector<XrCompositionLayerQuad*> resetQuadLayers{};
        std::vector<std::vector<XrCompositionLayerProjectionView>*> resetViews{};

        // use pose cache for reverse calculation 
        for (uint32_t i = 0; i < chainFrameEndInfo.layerCount; i++)
        {
            XrCompositionLayerBaseHeader baseHeader = *chainFrameEndInfo.layers[i];
            XrCompositionLayerBaseHeader* resetBaseHeader{nullptr};
            if (XR_TYPE_COMPOSITION_LAYER_PROJECTION == baseHeader.type)
            {
                DebugLog("xrEndFrame: projection layer %u, space: %u\n", i, baseHeader.space);

                const auto* projectionLayer =
                    reinterpret_cast<const XrCompositionLayerProjection*>(chainFrameEndInfo.layers[i]);

                TraceLoggingWrite(g_traceProvider,
                                  "xrEndFrame_Layer",
                                  TLArg("ProjectionLayer", "Type"),
                                  TLArg(projectionLayer->layerFlags, "Flags"),
                                  TLPArg(projectionLayer->space, "Space"));

                auto projectionViews = new std::vector<XrCompositionLayerProjectionView>{};
                resetViews.push_back(projectionViews);
                projectionViews->resize(projectionLayer->viewCount);
                memcpy(projectionViews->data(),
                       projectionLayer->views,
                       projectionLayer->viewCount * sizeof(XrCompositionLayerProjectionView));

                TraceLoggingWrite(g_traceProvider,
                                  "xrEndFrame_View",
                                  TLArg("Reversed_Manipulation", "Type"),
                                  TLArg(xr::ToString(reversedManipulation).c_str(), "Pose"));

                for (uint32_t j = 0; j < projectionLayer->viewCount; j++)
                {
                    TraceLoggingWrite(
                        g_traceProvider,
                        "xrEndFrame_View",
                        TLArg("View_Before", "Type"),
                        TLArg(xr::ToString((*projectionViews)[j].pose).c_str(), "Pose"),
                        TLArg(j, "Index"),
                        TLPArg((*projectionViews)[j].subImage.swapchain, "Swapchain"),
                        TLArg((*projectionViews)[j].subImage.imageArrayIndex, "ImageArrayIndex"),
                        TLArg(xr::ToString((*projectionViews)[j].subImage.imageRect).c_str(), "ImageRect"),
                        TLArg(xr::ToString((*projectionViews)[j].fov).c_str(), "Fov"));

                    XrPosef reversedEyePose = m_UseEyeCache
                                                  ? cachedEyePoses[j]
                                                  : Pose::Multiply((*projectionViews)[j].pose, reversedManipulation);
                    (*projectionViews)[j].pose = reversedEyePose;

                    TraceLoggingWrite(g_traceProvider,
                                      "xrEndFrame_View",
                                      TLArg("View_After", "Type"),
                                      TLArg(xr::ToString((*projectionViews)[j].pose).c_str(), "Pose"),
                                      TLArg(j, "Index"));
                }
            
                // create layer with reset view poses
                const auto resetProjectionLayer = new XrCompositionLayerProjection{projectionLayer->type,
                                                                                   projectionLayer->next,
                                                                                   projectionLayer->layerFlags,
                                                                                   projectionLayer->space,
                                                                                   projectionLayer->viewCount,
                                                                                   projectionViews->data()};
                resetProjectionLayers.push_back(resetProjectionLayer);
                resetBaseHeader = reinterpret_cast<XrCompositionLayerBaseHeader*>(resetProjectionLayer);
            }
            else if (XR_TYPE_COMPOSITION_LAYER_QUAD == baseHeader.type && !isViewSpace(baseHeader.space))
            {
                // compensate quad layers unless they are relative to view space
                DebugLog("xrEndFrame: quad layer %u, space: %u\n", i, baseHeader.space);

                const auto* quadLayer = reinterpret_cast<const XrCompositionLayerQuad*>(chainFrameEndInfo.layers[i]);

               TraceLoggingWrite(g_traceProvider,
                                  "xrEndFrame_Layer",
                                  TLArg("QuadLayer", "Type"),
                                  TLArg(quadLayer->layerFlags, "Flags"),
                                  TLPArg(quadLayer->space, "Space"),
                                  TLArg(xr::ToString(quadLayer->pose).c_str(), "Pose"));

                // apply reverse manipulation to quad layer pose
                XrPosef resetPose = Pose::Multiply(quadLayer->pose, reversedManipulation);

                TraceLoggingWrite(g_traceProvider,
                                  "xrEndFrame_Layer",
                                  TLArg("QuadLayer_After", "Type"),
                                  TLArg(xr::ToString(resetPose).c_str(), "Pose"));

                // create quad layer with reset pose
                auto* const resetQuadLayer = new XrCompositionLayerQuad{quadLayer->type,
                                                                        quadLayer->next,
                                                                        quadLayer->layerFlags,
                                                                        quadLayer->space,
                                                                        quadLayer->eyeVisibility,
                                                                        quadLayer->subImage,
                                                                        resetPose,
                                                                        quadLayer->size};
                resetQuadLayers.push_back(resetQuadLayer);
                resetBaseHeader = reinterpret_cast<XrCompositionLayerBaseHeader*>(resetQuadLayer);
            }
            if (resetBaseHeader)
            {
                resetLayers.push_back(resetBaseHeader);
            }
            else
            {
                resetLayers.push_back(chainFrameEndInfo.layers[i]);
            }
        }
        m_Input->HandleKeyboardInput(chainFrameEndInfo.displayTime);

        XrFrameEndInfo resetFrameEndInfo{chainFrameEndInfo.type,
                                         chainFrameEndInfo.next,
                                         chainFrameEndInfo.displayTime,
                                         chainFrameEndInfo.environmentBlendMode,
                                         chainFrameEndInfo.layerCount,
                                         resetLayers.data()};

        XrResult result = OpenXrApi::xrEndFrame(session, &resetFrameEndInfo);

        // clean up memory
        for (auto projection : resetProjectionLayers)
        {
            delete projection;
        }
        for (auto quad : resetQuadLayers)
        {
            delete quad;
        }
        for (auto views : resetViews)
        {
            delete views;
        }

        return result;
    }

    bool OpenXrLayer::GetStageToLocalSpace(const XrTime time, XrPosef& pose)
    {
        if (m_StageSpace == XR_NULL_HANDLE)
        {
            LazyInit(time);
        }
        if (m_StageSpace != XR_NULL_HANDLE)
        {
            XrSpaceLocation location{XR_TYPE_SPACE_LOCATION, nullptr};
            if (XR_SUCCEEDED(xrLocateSpace(m_StageSpace, m_ReferenceSpace, time, &location)))
            {
                if (Pose::IsPoseValid(location.locationFlags))
                {
                    DebugLog("local space to stage space: %s\n", xr::ToString(location.pose).c_str()); 
                    pose = location.pose;
                    TraceLoggingWrite(g_traceProvider,
                                      "LocateLocalInStageSpace",
                                      TLArg(xr::ToString(pose).c_str(), "StageToLocalPose"));
                    return true;
                }
                else
                {
                    ErrorLog("pose of local space in stage space not valid. locationFlags: %d\n",
                             location.locationFlags);
                }
            }
            else
            {
                ErrorLog("unable to locate local reference space in stage reference space\n");
            }
        }
        else
        {
            ErrorLog("stage reference space not initialized\n");
        }
        return false;
    }

    void OpenXrLayer::RequestCurrentInteractionProfile()
    {
        XrPath path;
        const std::string topLevel(m_ViveTracker.active ? m_ViveTracker.role
                                                            : "/user/hand/" + GetConfig()->GetControllerSide());
        XrInteractionProfileState profileState{XR_TYPE_INTERACTION_PROFILE_STATE, NULL, XR_NULL_PATH};
        try
        {
            CHECK_XRCMD(xrStringToPath(GetXrInstance(), topLevel.c_str(), &path));
            xrGetCurrentInteractionProfile(m_Session, path, &profileState);
        }
        catch (std::exception& e)
        {
            ErrorLog("%s: encountered exception: %s", __FUNCTION__, e.what());
        }
    }

    // private
    bool OpenXrLayer::isSystemHandled(XrSystemId systemId) const
    {
        return systemId == m_systemId;
    }

    bool OpenXrLayer::isSessionHandled(XrSession session) const
    {
        return session == m_Session;
    }

    bool OpenXrLayer::isViewSpace(XrSpace space) const
    {
        return m_ViewSpaces.contains(space);
    }

    uint32_t OpenXrLayer::GetNumViews() const
    {
        return XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MONO == m_ViewConfigType                                ? 1
               : XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO == m_ViewConfigType                            ? 2
               : XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO == m_ViewConfigType                        ? 4
               : XR_VIEW_CONFIGURATION_TYPE_SECONDARY_MONO_FIRST_PERSON_OBSERVER_MSFT == m_ViewConfigType ? 1
                                                                                                          : 0;
    }

    void OpenXrLayer::CreateTrackerAction()
    {
        if (m_PhysicalEnabled)
        {
            XrActionSetCreateInfo actionSetCreateInfo{XR_TYPE_ACTION_SET_CREATE_INFO, nullptr};
            strcpy_s(actionSetCreateInfo.actionSetName, "general_tracker_set");
            strcpy_s(actionSetCreateInfo.localizedActionSetName, "General Tracker Set");
            actionSetCreateInfo.priority = 0;
            if (!XR_SUCCEEDED(xrCreateActionSet(GetXrInstance(), &actionSetCreateInfo, &m_ActionSet)))
            {
                ErrorLog("%s: unable to create action set\n", __FUNCTION__);
            }
            TraceLoggingWrite(g_traceProvider,
                              "OpenXrLayer::CreateTrackerAction",
                              TLPArg(m_ActionSet, "xrCreateActionSet"));

            XrActionCreateInfo actionCreateInfo{XR_TYPE_ACTION_CREATE_INFO, nullptr};
            strcpy_s(actionCreateInfo.actionName, "general_tracker");
            strcpy_s(actionCreateInfo.localizedActionName, "General Tracker");
            actionCreateInfo.actionType = XR_ACTION_TYPE_POSE_INPUT;
            XrPath viveRolePath;
            if (m_ViveTracker.active)
            {
                CHECK_XRCMD(xrStringToPath(GetXrInstance(), m_ViveTracker.role.c_str(), &viveRolePath));
                actionCreateInfo.countSubactionPaths = 1;
                actionCreateInfo.subactionPaths = &viveRolePath;
            }
            else
            {
                actionCreateInfo.countSubactionPaths = 0;
            }
            if (XR_FAILED(xrCreateAction(m_ActionSet, &actionCreateInfo, &m_TrackerPoseAction)))
            {
                ErrorLog("%s: unable to create action\n", __FUNCTION__);
            }
            TraceLoggingWrite(g_traceProvider,
                              "OpenXrLayer::CreateTrackerAction",
                              TLPArg(m_TrackerPoseAction, "xrCreateAction"));
        }
    }

    void OpenXrLayer::CreateTrackerActionSpace()
    {
        if (m_PhysicalEnabled)
        {
            XrActionSpaceCreateInfo actionSpaceCreateInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO, nullptr};
            actionSpaceCreateInfo.action = m_TrackerPoseAction;
            m_ViveTracker.active
                ? CHECK_XRCMD(
                      xrStringToPath(GetXrInstance(), m_ViveTracker.role.c_str(), &actionSpaceCreateInfo.subactionPath))
                : actionSpaceCreateInfo.subactionPath = XR_NULL_PATH;
            actionSpaceCreateInfo.poseInActionSpace = Pose::Identity();
            if (XR_FAILED(GetInstance()->xrCreateActionSpace(m_Session, &actionSpaceCreateInfo, &m_TrackerSpace)))
            {
                ErrorLog("%s: unable to create action space\n", __FUNCTION__);
            }
            TraceLoggingWrite(g_traceProvider,
                              "OpenXrLayer::CreateTrackerActionSpace",
                              TLPArg(m_TrackerSpace, "xrCreateActionSpace"));
        }
    }

    bool OpenXrLayer::LazyInit(const XrTime time)
    {
        bool success = true;
        if (m_ReferenceSpace == XR_NULL_HANDLE)
        {
            Log("reference space created during lazy init\n");
            // Create a reference space.
            XrReferenceSpaceCreateInfo referenceSpaceCreateInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO, nullptr};
            referenceSpaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
            referenceSpaceCreateInfo.poseInReferenceSpace = Pose::Identity();
            TraceLoggingWrite(g_traceProvider, "OpenXrTracker::LazyInit", TLPArg("Executed", "xrCreateReferenceSpaceLocal"));
            if (!XR_SUCCEEDED(xrCreateReferenceSpace(m_Session, &referenceSpaceCreateInfo, &m_ReferenceSpace)))
            {
                ErrorLog("%s: xrCreateReferenceSpace failed\n", __FUNCTION__);
                success = false;
            }
        }
        std::string trackerType;
        if (m_StageSpace == XR_NULL_HANDLE && GetConfig()->GetString(Cfg::TrackerType, trackerType) &&
            ("yaw" == trackerType || "srs" == trackerType || "flypt" == trackerType))
        {
            Log("reference space created during lazy init\n");
            // Create a reference space.
            XrReferenceSpaceCreateInfo referenceSpaceCreateInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO, nullptr};
            referenceSpaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
            referenceSpaceCreateInfo.poseInReferenceSpace = Pose::Identity();
            TraceLoggingWrite(g_traceProvider,
                              "OpenXrTracker::LazyInit",
                              TLPArg("Executed", "xrCreateReferenceSpaceStage"));
            if (!XR_SUCCEEDED(xrCreateReferenceSpace(m_Session, &referenceSpaceCreateInfo, &m_StageSpace)))
            {
                ErrorLog("%s: xrCreateReferenceSpace failed\n", __FUNCTION__);
            }
        }
        
        if (m_PhysicalEnabled && !m_ActionSetAttached && GetConfig()->GetString(Cfg::TrackerType, trackerType) &&
            ("controller" == trackerType || "vive" == trackerType))
        {
            std::vector<XrActionSet> actionSets;
            const XrSessionActionSetsAttachInfo actionSetAttachInfo{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO,
                                                                    nullptr,
                                                                    0,
                                                                    actionSets.data()};
            TraceLoggingWrite(g_traceProvider,
                              "OpenXrLayer::LazyInit",
                              TLPArg("Executed", "xrAttachSessionActionSets"));
            if (XR_SUCCEEDED(xrAttachSessionActionSets(m_Session, &actionSetAttachInfo)))
            {
                Log("action set attached during lazy init\n");
                m_ActionSetAttached = true;
            }
            else
            {
                ErrorLog("%s: xrAttachSessionActionSets failed\n", __FUNCTION__);
                success = false;
            }
        }
        if (!m_Tracker->LazyInit(time))
        {
            success = false;
        }
        return success;
    }

    std::string OpenXrLayer::getXrPath(const XrPath path)
    {
        char buf[XR_MAX_PATH_LENGTH];
        uint32_t count;
        CHECK_XRCMD(GetInstance()->xrPathToString(GetInstance()->GetXrInstance(), path, sizeof(buf), &count, buf));
        std::string str;
        str.assign(buf, count - 1);
        return str;
    }

    bool OpenXrLayer::TestRotation(XrPosef* pose, const XrTime time, const bool reverse) const
    {
        // save current location
        const XrVector3f pos = pose->position;

        // determine rotation angle
        const int64_t milliseconds = ((time - m_TestRotStart) / 1000000) % 10000;
        float angle = static_cast<float>(M_PI) * 0.0002f * milliseconds;
        if (reverse)
        {
            angle = -angle;
        }

        // remove translation to rotate around center
        pose->position = {0, 0, 0};
        StoreXrPose(pose, XMMatrixMultiply(LoadXrPose(*pose), DirectX::XMMatrixRotationRollPitchYaw(0.f, angle, 0.f)));
        // reapply translation
        pose->position = pos;

        return true;
    }

    std::unique_ptr<OpenXrLayer> g_instance = nullptr;

    OpenXrApi* GetInstance()
    {
        if (!g_instance)
        {
            g_instance = std::make_unique<OpenXrLayer>();
        }
        return g_instance.get();
    }

    void ResetInstance()
    {
        g_instance.reset();
    }

} // namespace motion_compensation_layer

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        TraceLoggingRegister(motion_compensation_layer::log::g_traceProvider);
        break;

    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}
