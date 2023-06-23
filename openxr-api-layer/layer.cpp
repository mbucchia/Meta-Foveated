// MIT License
//
// Copyright(c) 2022-2023 Matthieu Bucchianeri
//
// Based on https://github.com/mbucchia/OpenXR-Layer-Template.
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
#include <log.h>
#include <util.h>
#include <utils/graphics.h>

#define A_CPU
#include <ffx_a.h>
#include <ffx_cas.h>

#include "views.h"

#include <ProjectionVS.h>
#include <ProjectionPS.h>
#include <SharpeningCS.h>

namespace openxr_api_layer {

    using namespace log;
    using namespace xr::math;
    using namespace openxr_api_layer::utils;

    // Initialize these vectors with arrays of extensions to block and implicitly request for the instance.
    const std::vector<std::string> blockedExtensions = {XR_VARJO_QUAD_VIEWS_EXTENSION_NAME,
                                                        XR_VARJO_FOVEATED_RENDERING_EXTENSION_NAME};
    const std::vector<std::string> implicitExtensions = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME,
                                                         XR_FB_EYE_TRACKING_SOCIAL_EXTENSION_NAME};

    struct ProjectionVSConstants {
        alignas(16) DirectX::XMFLOAT4X4 focusProjection;
    };

    struct ProjectionPSConstants {
        alignas(4) float smoothingArea;
        alignas(4) bool ignoreAlpha;
        alignas(4) bool isUnpremultipliedAlpha;
        alignas(4) bool debugFocusView;
    };

    struct SharpeningCSConstants {
        alignas(4) uint32_t Const0[4];
        alignas(4) uint32_t Const1[4];
    };

    // This class implements our API layer.
    class OpenXrLayer : public openxr_api_layer::OpenXrApi {
      public:
        OpenXrLayer() = default;
        ~OpenXrLayer() = default;

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrGetInstanceProcAddr
        XrResult xrGetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function) override {
            TraceLoggingWrite(g_traceProvider,
                              "xrGetInstanceProcAddr",
                              TLXArg(instance, "Instance"),
                              TLArg(name, "Name"),
                              TLArg(m_bypassApiLayer, "Bypass"));

            XrResult result = m_bypassApiLayer ? m_xrGetInstanceProcAddr(instance, name, function)
                                               : OpenXrApi::xrGetInstanceProcAddr(instance, name, function);

            TraceLoggingWrite(g_traceProvider, "xrGetInstanceProcAddr", TLPArg(*function, "Function"));

            return result;
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrCreateInstance
        XrResult xrCreateInstance(const XrInstanceCreateInfo* createInfo) override {
            if (createInfo->type != XR_TYPE_INSTANCE_CREATE_INFO) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            // Needed to resolve the requested function pointers.
            OpenXrApi::xrCreateInstance(createInfo);

            // Dump the application name, OpenXR runtime information and other useful things for debugging.
            TraceLoggingWrite(g_traceProvider,
                              "xrCreateInstance",
                              TLArg(xr::ToString(createInfo->applicationInfo.apiVersion).c_str(), "ApiVersion"),
                              TLArg(createInfo->applicationInfo.applicationName, "ApplicationName"),
                              TLArg(createInfo->applicationInfo.applicationVersion, "ApplicationVersion"),
                              TLArg(createInfo->applicationInfo.engineName, "EngineName"),
                              TLArg(createInfo->applicationInfo.engineVersion, "EngineVersion"),
                              TLArg(createInfo->createFlags, "CreateFlags"));
            Log(fmt::format("Application: {}\n", createInfo->applicationInfo.applicationName));

            for (uint32_t i = 0; i < createInfo->enabledApiLayerCount; i++) {
                TraceLoggingWrite(
                    g_traceProvider, "xrCreateInstance", TLArg(createInfo->enabledApiLayerNames[i], "ApiLayerName"));
            }

            // Bypass the extension unless the app might request quad views.
            bool requestedQuadViews = false;
            bool requestedD3D11 = false;
            for (uint32_t i = 0; i < createInfo->enabledExtensionCount; i++) {
                const std::string_view ext(createInfo->enabledExtensionNames[i]);
                TraceLoggingWrite(g_traceProvider, "xrCreateInstance", TLArg(ext.data(), "ExtensionName"));
                if (ext == XR_VARJO_QUAD_VIEWS_EXTENSION_NAME) {
                    requestedQuadViews = true;
                } else if (ext == XR_VARJO_FOVEATED_RENDERING_EXTENSION_NAME) {
                    m_requestedFoveatedRendering = true;
                } else if (ext == XR_KHR_D3D11_ENABLE_EXTENSION_NAME) {
                    requestedD3D11 = true;
                }
            }

            // We only support D3D11 at the moment.
            m_bypassApiLayer = !(requestedQuadViews && requestedD3D11);
            if (m_bypassApiLayer) {
                Log(fmt::format("{} layer will be bypassed\n", LayerName));
                return XR_SUCCESS;
            }

            XrInstanceProperties instanceProperties = {XR_TYPE_INSTANCE_PROPERTIES};
            CHECK_XRCMD(OpenXrApi::xrGetInstanceProperties(GetXrInstance(), &instanceProperties));
            const auto runtimeName = fmt::format("{} {}.{}.{}",
                                                 instanceProperties.runtimeName,
                                                 XR_VERSION_MAJOR(instanceProperties.runtimeVersion),
                                                 XR_VERSION_MINOR(instanceProperties.runtimeVersion),
                                                 XR_VERSION_PATCH(instanceProperties.runtimeVersion));
            TraceLoggingWrite(g_traceProvider, "xrCreateInstance", TLArg(runtimeName.c_str(), "RuntimeName"));
            Log(fmt::format("Using OpenXR runtime: {}\n", runtimeName));

            // Parse the configuration.
            LoadConfiguration();

            // Game-specific quirks.
            m_needFocusFovCorrectionQuirk = GetApplicationName() == "DCS World";

            TraceLoggingWrite(g_traceProvider,
                              "xrCreateInstance",
                              TLArg(m_peripheralPixelDensity, "PeripheralResolutionFactor"),
                              TLArg(m_focusPixelDensity, "FocusResolutionFactor"),
                              TLArg(m_horizontalFovSection[0], "FixedHorizontalSection"),
                              TLArg(m_verticalFovSection[0], "FixedVerticalSection"),
                              TLArg(m_horizontalFovSection[1], "FoveatedHorizontalSection"),
                              TLArg(m_verticalFovSection[1], "FoveatedVerticalSection"),
                              TLArg(m_verticalFocusBias, "VerticalFocusBias"),
                              TLArg(m_preferFoveatedRendering, "PreferFoveatedRendering"),
                              TLArg(m_smoothenFocusViewEdges, "SmoothenEdges"),
                              TLArg(m_sharpenFocusView, "SharpenFocusView"),
                              TLArg(m_useTurboMode, "TurboMode"));

            return XR_SUCCESS;
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrGetSystem
        XrResult xrGetSystem(XrInstance instance, const XrSystemGetInfo* getInfo, XrSystemId* systemId) override {
            if (getInfo->type != XR_TYPE_SYSTEM_GET_INFO) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            TraceLoggingWrite(g_traceProvider,
                              "xrGetSystem",
                              TLXArg(instance, "Instance"),
                              TLArg(xr::ToCString(getInfo->formFactor), "FormFactor"));

            const XrResult result = OpenXrApi::xrGetSystem(instance, getInfo, systemId);

            if (XR_SUCCEEDED(result) && getInfo->formFactor == XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY) {
                // Check if the system supports eye tracking.
                XrSystemEyeTrackingPropertiesFB eyeTrackingProperties{XR_TYPE_SYSTEM_EYE_TRACKING_PROPERTIES_FB};
                eyeTrackingProperties.supportsEyeTracking = XR_FALSE;
                XrSystemProperties systemProperties{XR_TYPE_SYSTEM_PROPERTIES};
                systemProperties.next = &eyeTrackingProperties;
                CHECK_XRCMD(OpenXrApi::xrGetSystemProperties(instance, *systemId, &systemProperties));
                TraceLoggingWrite(g_traceProvider,
                                  "xrGetSystem",
                                  TLArg(systemProperties.systemName, "SystemName"),
                                  TLArg(eyeTrackingProperties.supportsEyeTracking, "SupportsEyeTracking"));

                m_isEyeTrackingAvailable = m_debugSimulateTracking || eyeTrackingProperties.supportsEyeTracking;

                if (m_debugForceNoFoveated) {
                    m_isEyeTrackingAvailable = false;
                }

                static bool wasSystemLogged = false;
                if (!wasSystemLogged) {
                    Log(fmt::format("Using OpenXR system: {}\n", systemProperties.systemName));
                    Log(fmt::format("Eye tracking is {}\n", m_isEyeTrackingAvailable ? "supported" : "not supported"));
                    wasSystemLogged = true;
                }
            }

            TraceLoggingWrite(g_traceProvider, "xrGetSystem", TLArg((int)*systemId, "SystemId"));

            return result;
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrGetSystemProperties
        XrResult xrGetSystemProperties(XrInstance instance,
                                       XrSystemId systemId,
                                       XrSystemProperties* properties) override {
            TraceLoggingWrite(g_traceProvider,
                              "xrGetSystemProperties",
                              TLXArg(instance, "Instance"),
                              TLArg((int)systemId, "SystemId"));

            const XrResult result = OpenXrApi::xrGetSystemProperties(instance, systemId, properties);

            if (XR_SUCCEEDED(result)) {
                if (m_requestedFoveatedRendering) {
                    XrSystemFoveatedRenderingPropertiesVARJO* foveatedProperties =
                        reinterpret_cast<XrSystemFoveatedRenderingPropertiesVARJO*>(properties->next);
                    while (foveatedProperties) {
                        if (foveatedProperties->type == XR_TYPE_SYSTEM_FOVEATED_RENDERING_PROPERTIES_VARJO) {
                            foveatedProperties->supportsFoveatedRendering =
                                m_isEyeTrackingAvailable ? XR_TRUE : XR_FALSE;
                            break;
                        }
                        foveatedProperties =
                            reinterpret_cast<XrSystemFoveatedRenderingPropertiesVARJO*>(foveatedProperties->next);
                    }
                }
            }

            return result;
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrEnumerateViewConfigurations
        XrResult xrEnumerateViewConfigurations(XrInstance instance,
                                               XrSystemId systemId,
                                               uint32_t viewConfigurationTypeCapacityInput,
                                               uint32_t* viewConfigurationTypeCountOutput,
                                               XrViewConfigurationType* viewConfigurationTypes) override {
            TraceLoggingWrite(g_traceProvider,
                              "xrEnumerateViewConfigurations",
                              TLXArg(instance, "Instance"),
                              TLArg((int)systemId, "SystemId"),
                              TLArg(viewConfigurationTypeCapacityInput, "ViewConfigurationTypeCapacityInput"));

            XrResult result = XR_ERROR_RUNTIME_FAILURE;
            if (viewConfigurationTypeCapacityInput) {
                result = OpenXrApi::xrEnumerateViewConfigurations(instance,
                                                                  systemId,
                                                                  viewConfigurationTypeCapacityInput - 1,
                                                                  viewConfigurationTypeCountOutput,
                                                                  viewConfigurationTypes + 1);
                if (XR_SUCCEEDED(result)) {
                    // Prepend (since we prefer quad views).
                    viewConfigurationTypes[0] = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO;
                    (*viewConfigurationTypeCountOutput)++;
                }
            } else {
                result = OpenXrApi::xrEnumerateViewConfigurations(
                    instance, systemId, 0, viewConfigurationTypeCountOutput, nullptr);
                if (XR_SUCCEEDED(result)) {
                    (*viewConfigurationTypeCountOutput)++;
                }
            }

            if (XR_SUCCEEDED(result)) {
                TraceLoggingWrite(g_traceProvider,
                                  "xrEnumerateViewConfigurations",
                                  TLArg(*viewConfigurationTypeCountOutput, "ViewConfigurationTypeCountOutput"));

                if (viewConfigurationTypeCapacityInput && viewConfigurationTypes) {
                    for (uint32_t i = 0; i < *viewConfigurationTypeCountOutput; i++) {
                        TraceLoggingWrite(g_traceProvider,
                                          "xrEnumerateViewConfigurations",
                                          TLArg(xr::ToCString(viewConfigurationTypes[i]), "ViewConfigurationType"));
                    }
                }
            }

            return result;
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrEnumerateViewConfigurationViews
        XrResult xrEnumerateViewConfigurationViews(XrInstance instance,
                                                   XrSystemId systemId,
                                                   XrViewConfigurationType viewConfigurationType,
                                                   uint32_t viewCapacityInput,
                                                   uint32_t* viewCountOutput,
                                                   XrViewConfigurationView* views) override {
            TraceLoggingWrite(g_traceProvider,
                              "xrEnumerateViewConfigurationViews",
                              TLXArg(instance, "Instance"),
                              TLArg((int)systemId, "SystemId"),
                              TLArg(viewCapacityInput, "ViewCapacityInput"),
                              TLArg(xr::ToCString(viewConfigurationType), "ViewConfigurationType"));

            XrResult result = XR_ERROR_RUNTIME_FAILURE;
            if (viewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO) {
                if (viewCapacityInput) {
                    XrViewConfigurationView stereoViews[xr::StereoView::Count]{{XR_TYPE_VIEW_CONFIGURATION_VIEW},
                                                                               {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
                    if (viewCapacityInput >= xr::QuadView::Count) {
                        result = OpenXrApi::xrEnumerateViewConfigurationViews(instance,
                                                                              systemId,
                                                                              XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                                              xr::StereoView::Count,
                                                                              viewCountOutput,
                                                                              stereoViews);
                    } else {
                        result = XR_ERROR_SIZE_INSUFFICIENT;
                    }

                    if (XR_SUCCEEDED(result)) {
                        *viewCountOutput = xr::QuadView::Count;

                        for (uint32_t i = 0; i < *viewCountOutput; i++) {
                            if (views[i].type != XR_TYPE_VIEW_CONFIGURATION_VIEW) {
                                return XR_ERROR_VALIDATION_FAILURE;
                            }
                        }

                        // Make sure we have the prerequisite data to compute the resolutions we need.
                        if (!m_debugDeferPopulateFovTable) {
                            populateFovTables(systemId);
                        }

                        // Override default to specify whether foveated rendering is desired when the application does
                        // not specify.
                        bool foveatedRenderingActive = m_isEyeTrackingAvailable && m_preferFoveatedRendering;

                        // When foveated rendering extension is active, look whether the application is requesting it
                        // for the views. The spec is a little questionable and calls for each view to have the flag
                        // specified. Here we check that at least one view has the flag on.
                        if (m_requestedFoveatedRendering) {
                            for (uint32_t i = 0; i < *viewCountOutput; i++) {
                                const XrFoveatedViewConfigurationViewVARJO* foveatedViewConfiguration =
                                    reinterpret_cast<const XrFoveatedViewConfigurationViewVARJO*>(views[i].next);
                                while (foveatedViewConfiguration) {
                                    if (foveatedViewConfiguration->type ==
                                        XR_TYPE_FOVEATED_VIEW_CONFIGURATION_VIEW_VARJO) {
                                        foveatedRenderingActive = foveatedRenderingActive ||
                                                                  foveatedViewConfiguration->foveatedRenderingActive;
                                        break;
                                    }
                                    foveatedViewConfiguration =
                                        reinterpret_cast<const XrFoveatedViewConfigurationViewVARJO*>(
                                            foveatedViewConfiguration->next);
                                }
                            }

                            TraceLoggingWrite(g_traceProvider,
                                              "xrEnumerateViewConfigurationViews",
                                              TLArg(foveatedRenderingActive, "FoveatedRenderingActive"));
                        }

                        const float basePixelDensity = stereoViews[xr::StereoView::Left].recommendedImageRectWidth /
                                                       (-m_cachedEyeFov[xr::StereoView::Left].angleLeft +
                                                        m_cachedEyeFov[xr::StereoView::Left].angleRight);

                        // When using quad views, we use 2 peripheral views with lower pixel densities, and 2 focus
                        // views with higher pixel densities.
                        for (uint32_t i = 0; i < *viewCountOutput; i++) {
                            uint32_t referenceFovIndex = i;
                            float pixelDensityMultiplier = m_peripheralPixelDensity;
                            if (i >= xr::StereoView::Count) {
                                pixelDensityMultiplier = m_focusPixelDensity;
                                if (foveatedRenderingActive) {
                                    referenceFovIndex = i + 2;
                                }
                            }

                            float newWidth;
                            if (!m_debugDeferPopulateFovTable) {
                                const float horizontalFov = (-m_cachedEyeFov[referenceFovIndex].angleLeft +
                                                             m_cachedEyeFov[referenceFovIndex].angleRight);
                                newWidth = basePixelDensity * pixelDensityMultiplier * horizontalFov;
                            } else {
                                if (i < xr::StereoView::Count) {
                                    newWidth = pixelDensityMultiplier *
                                               stereoViews[i % xr::StereoView::Count].recommendedImageRectWidth;
                                } else {
                                    newWidth = pixelDensityMultiplier *
                                               m_horizontalFovSection[foveatedRenderingActive ? 1 : 0] *
                                               stereoViews[i % xr::StereoView::Count].recommendedImageRectWidth;
                                }
                            }
                            const float ratio =
                                (float)stereoViews[i % xr::StereoView::Count].recommendedImageRectHeight /
                                stereoViews[i % xr::StereoView::Count].recommendedImageRectWidth;
                            const float newHeight = newWidth * ratio;

                            views[i] = stereoViews[i % xr::StereoView::Count];
                            views[i].recommendedImageRectWidth =
                                std::min((uint32_t)newWidth, views[i].maxImageRectWidth);
                            views[i].recommendedImageRectHeight =
                                std::min((uint32_t)newHeight, views[i].maxImageRectHeight);
                        }

                        if (!m_loggedResolution) {
                            Log("Recommended peripheral resolution: %ux%u (%.3fx density)\n",
                                views[xr::StereoView::Left].recommendedImageRectWidth,
                                views[xr::StereoView::Left].recommendedImageRectHeight,
                                m_peripheralPixelDensity);
                            Log("Recommended focus resolution: %ux%u (%.3fx density)\n",
                                views[xr::QuadView::FocusLeft].recommendedImageRectWidth,
                                views[xr::QuadView::FocusLeft].recommendedImageRectHeight,
                                m_focusPixelDensity);
                            m_loggedResolution = true;
                        }
                    }
                } else {
                    result = OpenXrApi::xrEnumerateViewConfigurationViews(
                        instance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, viewCountOutput, nullptr);
                    if (XR_SUCCEEDED(result)) {
                        *viewCountOutput = xr::QuadView::Count;
                    }
                }
            } else {
                result = OpenXrApi::xrEnumerateViewConfigurationViews(
                    instance, systemId, viewConfigurationType, viewCapacityInput, viewCountOutput, views);
            }

            if (XR_SUCCEEDED(result)) {
                if (viewCapacityInput && views) {
                    for (uint32_t i = 0; i < *viewCountOutput; i++) {
                        TraceLoggingWrite(
                            g_traceProvider,
                            "xrEnumerateViewConfigurationViews",
                            TLArg(i, "ViewIndex"),
                            TLArg(views[i].maxImageRectWidth, "MaxImageRectWidth"),
                            TLArg(views[i].maxImageRectHeight, "MaxImageRectHeight"),
                            TLArg(views[i].maxSwapchainSampleCount, "MaxSwapchainSampleCount"),
                            TLArg(views[i].recommendedImageRectWidth, "RecommendedImageRectWidth"),
                            TLArg(views[i].recommendedImageRectHeight, "RecommendedImageRectHeight"),
                            TLArg(views[i].recommendedSwapchainSampleCount, "RecommendedSwapchainSampleCount"));
                    }
                }
            }

            return result;
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrEnumerateEnvironmentBlendModes
        XrResult xrEnumerateEnvironmentBlendModes(XrInstance instance,
                                                  XrSystemId systemId,
                                                  XrViewConfigurationType viewConfigurationType,
                                                  uint32_t environmentBlendModeCapacityInput,
                                                  uint32_t* environmentBlendModeCountOutput,
                                                  XrEnvironmentBlendMode* environmentBlendModes) override {
            TraceLoggingWrite(g_traceProvider,
                              "xrEnumerateEnvironmentBlendModes",
                              TLXArg(instance, "Instance"),
                              TLArg((int)systemId, "SystemId"),
                              TLArg(xr::ToCString(viewConfigurationType), "ViewConfigurationType"),
                              TLArg(environmentBlendModeCapacityInput, "EnvironmentBlendModeCapacityInput"));

            // We will implement quad views on top of stereo.
            if (viewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO) {
                viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            }

            const XrResult result = OpenXrApi::xrEnumerateEnvironmentBlendModes(instance,
                                                                                systemId,
                                                                                viewConfigurationType,
                                                                                environmentBlendModeCapacityInput,
                                                                                environmentBlendModeCountOutput,
                                                                                environmentBlendModes);

            if (XR_SUCCEEDED(result)) {
                TraceLoggingWrite(g_traceProvider,
                                  "xrEnumerateEnvironmentBlendModes",
                                  TLArg(*environmentBlendModeCountOutput, "EnvironmentBlendModeCountOutput"));

                if (environmentBlendModeCapacityInput && environmentBlendModes) {
                    for (uint32_t i = 0; i < *environmentBlendModeCountOutput; i++) {
                        TraceLoggingWrite(g_traceProvider,
                                          "xrEnumerateEnvironmentBlendModes",
                                          TLArg(xr::ToCString(environmentBlendModes[i]), "EnvironmentBlendMode"));
                    }
                }
            }

            return result;
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrGetViewConfigurationProperties
        XrResult xrGetViewConfigurationProperties(XrInstance instance,
                                                  XrSystemId systemId,
                                                  XrViewConfigurationType viewConfigurationType,
                                                  XrViewConfigurationProperties* configurationProperties) override {
            TraceLoggingWrite(g_traceProvider,
                              "xrGetViewConfigurationProperties",
                              TLXArg(instance, "Instance"),
                              TLArg((int)systemId, "SystemId"),
                              TLArg(xr::ToCString(viewConfigurationType), "ViewConfigurationType"));

            // We will implement quad views on top of stereo.
            const XrViewConfigurationType originalViewConfigurationType = viewConfigurationType;
            if (viewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO) {
                viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            }

            const XrResult result = OpenXrApi::xrGetViewConfigurationProperties(
                instance, systemId, viewConfigurationType, configurationProperties);

            if (XR_SUCCEEDED(result)) {
                if (originalViewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO) {
                    configurationProperties->viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO;
                }

                TraceLoggingWrite(
                    g_traceProvider,
                    "xrGetViewConfigurationProperties",
                    TLArg(xr::ToCString(configurationProperties->viewConfigurationType), "ViewConfigurationType"),
                    TLArg(!!configurationProperties->fovMutable, "FovMutable"));
            }

            return result;
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrCreateSession
        XrResult xrCreateSession(XrInstance instance,
                                 const XrSessionCreateInfo* createInfo,
                                 XrSession* session) override {
            if (createInfo->type != XR_TYPE_SESSION_CREATE_INFO) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            TraceLoggingWrite(g_traceProvider,
                              "xrCreateSession",
                              TLXArg(instance, "Instance"),
                              TLArg((int)createInfo->systemId, "SystemId"),
                              TLArg(createInfo->createFlags, "CreateFlags"));

            {
                const XrBaseInStructure* entry = reinterpret_cast<const XrBaseInStructure*>(createInfo->next);
                while (entry) {
                    if (entry->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
                        m_isSupportedGraphicsApi = true;
                    }
                    entry = entry->next;
                }
            }

            const XrResult result = OpenXrApi::xrCreateSession(instance, createInfo, session);

            if (XR_SUCCEEDED(result)) {
                TraceLoggingWrite(g_traceProvider, "xrCreateSession", TLXArg(*session, "Session"));

                m_systemId = createInfo->systemId;
            }

            return result;
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrDestroySession
        XrResult xrDestroySession(XrSession session) override {
            TraceLoggingWrite(g_traceProvider, "xrDestroySession", TLXArg(session, "Session"));

            // Wait for deferred frames to finish before teardown.
            if (m_asyncWaitPromise.valid()) {
                TraceLocalActivity(local);

                TraceLoggingWriteStart(local, "AsyncWaitNow");
                m_asyncWaitPromise.wait_for(5s);
                TraceLoggingWriteStop(local, "AsyncWaitNow");

                m_asyncWaitPromise = {};
            }

            const XrResult result = OpenXrApi::xrDestroySession(session);

            if (XR_SUCCEEDED(result)) {
                m_layerContextState.Reset();
                m_linearClampSampler.Reset();
                m_noDepthRasterizer.Reset();
                m_projectionVSConstants.Reset();
                m_projectionPSConstants.Reset();
                m_projectionVS.Reset();
                m_projectionPS.Reset();
                m_sharpeningCSConstants.Reset();
                m_sharpeningCS.Reset();

                m_applicationDevice.Reset();
                m_renderContext.Reset();

                m_gazeSpaces.clear();
                m_swapchains.clear();
            }

            return result;
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrBeginSession
        XrResult xrBeginSession(XrSession session, const XrSessionBeginInfo* beginInfo) override {
            if (beginInfo->type != XR_TYPE_SESSION_BEGIN_INFO) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            TraceLoggingWrite(
                g_traceProvider,
                "xrBeginSession",
                TLXArg(session, "Session"),
                TLArg(xr::ToCString(beginInfo->primaryViewConfigurationType), "PrimaryViewConfigurationType"));

            // We will implement quad views on top of stereo.
            XrSessionBeginInfo chainBeginInfo = *beginInfo;
            if (beginInfo->primaryViewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO) {
                // The concept of enumerating view configuration types and graphics API are decoupled.
                // We try to fail as gracefully as possible when we cannot support the configuration.
                if (!m_isSupportedGraphicsApi) {
                    ErrorLog("Session is using an unsupported graphics API\n");
                    return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
                }

                Log("Session is using quad views\n");
                m_useQuadViews = true;
                chainBeginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            }

            const XrResult result = OpenXrApi::xrBeginSession(session, &chainBeginInfo);

            if (XR_SUCCEEDED(result)) {
                // Make sure we have the prerequisite data to compute the views in subsequent calls.
                populateFovTables(m_systemId, session);

                if (m_isEyeTrackingAvailable) {
                    if (!m_debugSimulateTracking) {
                        XrEyeTrackerCreateInfoFB createInfo{XR_TYPE_EYE_TRACKER_CREATE_INFO_FB};
                        CHECK_XRCMD(OpenXrApi::xrCreateEyeTrackerFB(session, &createInfo, &m_eyeTrackerFB));

                        XrReferenceSpaceCreateInfo spaceCreateInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
                        spaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
                        spaceCreateInfo.poseInReferenceSpace = Pose::Identity();
                        CHECK_XRCMD(OpenXrApi::xrCreateReferenceSpace(session, &spaceCreateInfo, &m_viewSpace));
                    }
                }

                if (m_useQuadViews) {
                    if (m_smoothenFocusViewEdges) {
                        Log(fmt::format("Edge smoothing: {:.2f}\n", m_smoothenFocusViewEdges));
                    } else {
                        Log("Edge smoothing: Disabled\n");
                    }
                    if (m_sharpenFocusView) {
                        Log(fmt::format("Sharpening: {:.2f}\n", m_sharpenFocusView));
                    } else {
                        Log("Sharpening: Disabled\n");
                    }
                    Log(fmt::format("Turbo: {}\n", m_useTurboMode ? "Enabled" : "Disabled"));
                }
            }

            return result;
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrLocateViews
        XrResult xrLocateViews(XrSession session,
                               const XrViewLocateInfo* viewLocateInfo,
                               XrViewState* viewState,
                               uint32_t viewCapacityInput,
                               uint32_t* viewCountOutput,
                               XrView* views) override {
            if (viewLocateInfo->type != XR_TYPE_VIEW_LOCATE_INFO) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            TraceLoggingWrite(g_traceProvider,
                              "xrLocateViews",
                              TLXArg(session, "Session"),
                              TLArg(xr::ToCString(viewLocateInfo->viewConfigurationType), "ViewConfigurationType"),
                              TLArg(viewLocateInfo->displayTime, "DisplayTime"),
                              TLXArg(viewLocateInfo->space, "Space"),
                              TLArg(viewCapacityInput, "ViewCapacityInput"));

            // TODO: Conformance: viewConfigurationType must match the session.

            XrResult result = XR_ERROR_RUNTIME_FAILURE;
            if (viewLocateInfo->viewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO) {
                if (m_useQuadViews) {
                    XrViewLocateInfo chainViewLocateInfo = *viewLocateInfo;
                    chainViewLocateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;

                    if (viewCapacityInput) {
                        if (viewCapacityInput >= xr::QuadView::Count) {
                            result = OpenXrApi::xrLocateViews(session,
                                                              &chainViewLocateInfo,
                                                              viewState,
                                                              xr::StereoView::Count,
                                                              viewCountOutput,
                                                              views);
                        } else {
                            result = XR_ERROR_SIZE_INSUFFICIENT;
                        }

                        if (XR_SUCCEEDED(result)) {
                            *viewCountOutput = xr::QuadView::Count;

                            for (uint32_t i = 0; i < *viewCountOutput; i++) {
                                if (views[i].type != XR_TYPE_VIEW) {
                                    return XR_ERROR_VALIDATION_FAILURE;
                                }
                            }

                            if (viewState->viewStateFlags &
                                (XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
                                // Override default to specify whether foveated rendering is desired when the
                                // application does not specify.
                                bool foveatedRenderingActive = m_isEyeTrackingAvailable && m_preferFoveatedRendering;

                                if (m_requestedFoveatedRendering) {
                                    const XrViewLocateFoveatedRenderingVARJO* foveatedLocate =
                                        reinterpret_cast<const XrViewLocateFoveatedRenderingVARJO*>(
                                            viewLocateInfo->next);
                                    while (foveatedLocate) {
                                        if (foveatedLocate->type == XR_TYPE_VIEW_LOCATE_FOVEATED_RENDERING_VARJO) {
                                            foveatedRenderingActive = foveatedLocate->foveatedRenderingActive;
                                            break;
                                        }
                                        foveatedLocate = reinterpret_cast<const XrViewLocateFoveatedRenderingVARJO*>(
                                            foveatedLocate->next);
                                    }

                                    TraceLoggingWrite(g_traceProvider,
                                                      "xrLocateViews",
                                                      TLArg(foveatedRenderingActive, "FoveatedRenderingActive"));
                                }

                                // Query the eye tracker if needed.
                                bool isGazeValid = false;
                                XrVector3f gazeUnitVector{};
                                if (foveatedRenderingActive) {
                                    isGazeValid = getEyeGaze(
                                        viewLocateInfo->displayTime, false /* getStateOnly */, gazeUnitVector);
                                }

                                // Set up the focus view.
                                for (uint32_t i = xr::StereoView::Count; i < *viewCountOutput; i++) {
                                    const uint32_t stereoViewIndex = i - xr::StereoView::Count;

                                    views[i].pose = views[stereoViewIndex].pose;

                                    XrView viewForGazeProjection{};
                                    viewForGazeProjection.pose = m_cachedEyePoses[stereoViewIndex];
                                    viewForGazeProjection.fov = views[stereoViewIndex].fov;
                                    XrVector2f projectedGaze;
                                    if (!isGazeValid ||
                                        !ProjectPoint(viewForGazeProjection, gazeUnitVector, projectedGaze)) {
                                        views[i].fov = m_cachedEyeFov[i];

                                    } else {
                                        // Shift FOV according to the eye gaze.
                                        // We also widen the FOV when near the edges of the headset to make sure there's
                                        // enough overlap between the two eyes.
                                        const float MaxWidenAngle = 0.122173f; /* rad */
                                        constexpr float Deadzone = 0.15f;
                                        const XrVector2f centerOfFov{(projectedGaze.x + 1.f) / 2.f,
                                                                     (1.f - projectedGaze.y + m_verticalFocusBias) /
                                                                         2.f};
                                        const XrVector2f v = centerOfFov - m_centerOfFov[stereoViewIndex];
                                        const float distanceFromCenter = std::sqrt(v.x * v.x + v.y * v.y);
                                        const float widenHalfAngle =
                                            std::clamp(distanceFromCenter - Deadzone, 0.f, 0.5f) * MaxWidenAngle;
                                        XrFovf globalFov = m_cachedEyeFov[i % xr::StereoView::Count];
                                        std::tie(views[i].fov.angleLeft, views[i].fov.angleRight) =
                                            Fov::Lerp(std::make_pair(globalFov.angleLeft, globalFov.angleRight),
                                                      std::make_pair(m_cachedEyeFov[i + 2].angleLeft - widenHalfAngle,
                                                                     m_cachedEyeFov[i + 2].angleRight + widenHalfAngle),
                                                      centerOfFov.x);
                                        std::tie(views[i].fov.angleDown, views[i].fov.angleUp) =
                                            Fov::Lerp(std::make_pair(globalFov.angleDown, globalFov.angleUp),
                                                      std::make_pair(m_cachedEyeFov[i + 2].angleDown - widenHalfAngle,
                                                                     m_cachedEyeFov[i + 2].angleUp + widenHalfAngle),
                                                      centerOfFov.y);
                                    }
                                }

                                // Quirk for DCS World: the application does not pass the correct FOV for the focus
                                // views in xrEndFrame(). We must keep track of the correct values for each frame.
                                if (m_needFocusFovCorrectionQuirk) {
                                    std::unique_lock lock(m_focusFovMutex);

                                    m_focusFovForDisplayTime.insert_or_assign(
                                        viewLocateInfo->displayTime,
                                        std::make_pair(views[xr::QuadView::FocusLeft].fov,
                                                       views[xr::QuadView::FocusRight].fov));
                                }
                            }
                        }
                    } else {
                        result = OpenXrApi::xrLocateViews(
                            session, &chainViewLocateInfo, viewState, 0, viewCountOutput, nullptr);
                        if (XR_SUCCEEDED(result)) {
                            *viewCountOutput = xr::QuadView::Count;
                        }
                    }
                } else {
                    result = XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
                }
            } else {
                result = OpenXrApi::xrLocateViews(
                    session, viewLocateInfo, viewState, viewCapacityInput, viewCountOutput, views);
            }

            if (XR_SUCCEEDED(result)) {
                TraceLoggingWrite(g_traceProvider, "xrLocateViews", TLArg(*viewCountOutput, "ViewCountOutput"));

                if (viewCapacityInput && views) {
                    for (uint32_t i = 0; i < *viewCountOutput; i++) {
                        TraceLoggingWrite(g_traceProvider,
                                          "xrLocateViews",
                                          TLArg(i, "ViewIndex"),
                                          TLArg(xr::ToString(views[i].pose).c_str(), "Pose"),
                                          TLArg(xr::ToString(views[i].fov).c_str(), "Fov"));
                    }
                }
            }

            return result;
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrCreateSwapchain
        XrResult xrCreateSwapchain(XrSession session,
                                   const XrSwapchainCreateInfo* createInfo,
                                   XrSwapchain* swapchain) override {
            if (createInfo->type != XR_TYPE_SWAPCHAIN_CREATE_INFO) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            TraceLoggingWrite(g_traceProvider,
                              "xrCreateSwapchain",
                              TLXArg(session, "Session"),
                              TLArg(createInfo->arraySize, "ArraySize"),
                              TLArg(createInfo->width, "Width"),
                              TLArg(createInfo->height, "Height"),
                              TLArg(createInfo->createFlags, "CreateFlags"),
                              TLArg(createInfo->format, "Format"),
                              TLArg(createInfo->faceCount, "FaceCount"),
                              TLArg(createInfo->mipCount, "MipCount"),
                              TLArg(createInfo->sampleCount, "SampleCount"),
                              TLArg(createInfo->usageFlags, "UsageFlags"));

            const XrResult result = OpenXrApi::xrCreateSwapchain(session, createInfo, swapchain);

            if (XR_SUCCEEDED(result)) {
                TraceLoggingWrite(g_traceProvider, "xrCreateSwapchain", TLXArg(*swapchain, "Swapchain"));

                std::unique_lock lock(m_swapchainsMutex);
                Swapchain newEntry{};
                newEntry.createInfo = *createInfo;
                m_swapchains.insert_or_assign(*swapchain, std::move(newEntry));
            }

            return result;
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrDestroySwapchain
        XrResult xrDestroySwapchain(XrSwapchain swapchain) override {
            TraceLoggingWrite(g_traceProvider, "xrDestroySwapchain", TLXArg(swapchain, "Swapchain"));

            // In Turbo Mode, make sure there is no pending frame that may potentially hold onto the swapchain.
            {
                std::unique_lock lock(m_frameMutex);

                if (m_asyncWaitPromise.valid()) {
                    TraceLocalActivity(local);

                    TraceLoggingWriteStart(local, "AsyncWaitNow");
                    m_asyncWaitPromise.wait();
                    TraceLoggingWriteStop(local, "AsyncWaitNow");
                }
            }

            const XrResult result = OpenXrApi::xrDestroySwapchain(swapchain);

            if (XR_SUCCEEDED(result)) {
                std::unique_lock lock(m_swapchainsMutex);

                auto it = m_swapchains.find(swapchain);
                Swapchain& entry = it->second;
                for (uint32_t i = 0; i < xr::StereoView::Count; i++) {
                    if (entry.fullFovSwapchain[i] != XR_NULL_HANDLE) {
                        OpenXrApi::xrDestroySwapchain(entry.fullFovSwapchain[i]);
                    }
                }
                m_swapchains.erase(it);
            }

            return result;
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrAcquireSwapchainImage
        XrResult xrAcquireSwapchainImage(XrSwapchain swapchain,
                                         const XrSwapchainImageAcquireInfo* acquireInfo,
                                         uint32_t* index) override {
            TraceLoggingWrite(g_traceProvider, "xrAcquireSwapchainImage", TLXArg(swapchain, "Swapchain"));

            const XrResult result = OpenXrApi::xrAcquireSwapchainImage(swapchain, acquireInfo, index);

            if (XR_SUCCEEDED(result)) {
                TraceLoggingWrite(g_traceProvider, "xrAcquireSwapchainImage", TLArg(*index, "Index"));

                std::unique_lock lock(m_swapchainsMutex);
                Swapchain& entry = m_swapchains[swapchain];
                entry.acquiredIndex.push_back(*index);
            }

            return result;
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrReleaseSwapchainImage
        XrResult xrReleaseSwapchainImage(XrSwapchain swapchain,
                                         const XrSwapchainImageReleaseInfo* releaseInfo) override {
            TraceLoggingWrite(g_traceProvider, "xrReleaseSwapchainImage", TLXArg(swapchain, "Swapchain"));

            const XrResult result = OpenXrApi::xrReleaseSwapchainImage(swapchain, releaseInfo);

            if (XR_SUCCEEDED(result)) {
                std::unique_lock lock(m_swapchainsMutex);

                Swapchain& entry = m_swapchains[swapchain];
                entry.lastReleasedIndex = entry.acquiredIndex.front();
                entry.acquiredIndex.pop_front();
            }

            return result;
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrWaitFrame
        XrResult xrWaitFrame(XrSession session,
                             const XrFrameWaitInfo* frameWaitInfo,
                             XrFrameState* frameState) override {
            TraceLoggingWrite(g_traceProvider, "xrWaitFrame", TLXArg(session, "Session"));

            const auto lastFrameWaitTimestamp = m_lastFrameWaitTimestamp;
            m_lastFrameWaitTimestamp = std::chrono::steady_clock::now();

            XrResult result = XR_ERROR_RUNTIME_FAILURE;
            {
                std::unique_lock lock(m_frameMutex);

                if (m_asyncWaitPromise.valid()) {
                    TraceLoggingWrite(g_traceProvider, "AsyncWaitMode");

                    // In Turbo mode, we accept pipelining of exactly one frame.
                    if (m_asyncWaitPolled) {
                        TraceLocalActivity(local);

                        // On second frame poll, we must wait.
                        TraceLoggingWriteStart(local, "AsyncWaitNow");
                        m_asyncWaitPromise.wait();
                        TraceLoggingWriteStop(local, "AsyncWaitNow");
                    }
                    m_asyncWaitPolled = true;

                    // In Turbo mode, we don't actually wait, we make up a predicted time.
                    {
                        std::unique_lock lock(m_asyncWaitMutex);

                        frameState->predictedDisplayTime =
                            m_asyncWaitCompleted ? m_lastPredictedDisplayTime
                                                 : (m_lastPredictedDisplayTime +
                                                    (m_lastFrameWaitTimestamp - lastFrameWaitTimestamp).count());
                        frameState->predictedDisplayPeriod = m_lastPredictedDisplayPeriod;
                    }
                    frameState->shouldRender = XR_TRUE;

                    result = XR_SUCCESS;

                } else {
                    lock.unlock();
                    result = OpenXrApi::xrWaitFrame(session, frameWaitInfo, frameState);
                    lock.lock();

                    if (XR_SUCCEEDED(result)) {
                        // We must always store those values to properly handle transitions into Turbo Mode.
                        m_lastPredictedDisplayTime = frameState->predictedDisplayTime;
                        m_lastPredictedDisplayPeriod = frameState->predictedDisplayPeriod;
                    }
                }
            }

            if (XR_SUCCEEDED(result)) {
                // Per OpenXR spec, the predicted display must increase monotonically.
                frameState->predictedDisplayTime = std::max(frameState->predictedDisplayTime, m_waitedFrameTime + 1);

                // Record the predicted display time.
                m_waitedFrameTime = frameState->predictedDisplayTime;

                TraceLoggingWrite(g_traceProvider,
                                  "xrWaitFrame",
                                  TLArg(!!frameState->shouldRender, "ShouldRender"),
                                  TLArg(frameState->predictedDisplayTime, "PredictedDisplayTime"),
                                  TLArg(frameState->predictedDisplayPeriod, "PredictedDisplayPeriod"));
            }

            return result;
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrBeginFrame
        XrResult xrBeginFrame(XrSession session, const XrFrameBeginInfo* frameBeginInfo) override {
            TraceLoggingWrite(g_traceProvider, "xrBeginFrame", TLXArg(session, "Session"));

            XrResult result = XR_ERROR_RUNTIME_FAILURE;
            {
                std::unique_lock lock(m_frameMutex);

                if (m_asyncWaitPromise.valid()) {
                    // In turbo mode, we do nothing here.
                    TraceLoggingWrite(g_traceProvider, "AsyncWaitMode");
                    result = XR_SUCCESS;
                } else {
                    result = OpenXrApi::xrBeginFrame(session, frameBeginInfo);
                }
            }

            return result;
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrEndFrame
        XrResult xrEndFrame(XrSession session, const XrFrameEndInfo* frameEndInfo) override {
            if (frameEndInfo->type != XR_TYPE_FRAME_END_INFO) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            TraceLoggingWrite(g_traceProvider,
                              "xrEndFrame",
                              TLXArg(session, "Session"),
                              TLArg(frameEndInfo->displayTime, "DisplayTime"),
                              TLArg(xr::ToCString(frameEndInfo->environmentBlendMode), "EnvironmentBlendMode"));

            handleDebugKeys();

            // We will allocate structures to pass to the real xrEndFrame().
            std::vector<XrCompositionLayerProjection> projectionAllocator;
            std::vector<std::array<XrCompositionLayerProjectionView, xr::StereoView::Count>> projectionViewAllocator;
            std::vector<const XrCompositionLayerBaseHeader*> layers;

            // Ensure pointers within the collections remain stable.
            projectionAllocator.reserve(frameEndInfo->layerCount);
            projectionViewAllocator.reserve(frameEndInfo->layerCount);

            XrFrameEndInfo chainFrameEndInfo = *frameEndInfo;

            if (m_useQuadViews) {
                for (uint32_t i = 0; i < frameEndInfo->layerCount; i++) {
                    if (!frameEndInfo->layers[i]) {
                        return XR_ERROR_LAYER_INVALID;
                    }

                    if (frameEndInfo->layers[i]->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
                        const XrCompositionLayerProjection* proj =
                            reinterpret_cast<const XrCompositionLayerProjection*>(frameEndInfo->layers[i]);

                        TraceLoggingWrite(g_traceProvider,
                                          "xrEndFrame_Layer",
                                          TLArg(xr::ToCString(proj->type), "Type"),
                                          TLArg(proj->layerFlags, "Flags"),
                                          TLXArg(proj->space, "Space"));

                        if (proj->viewCount != xr::QuadView::Count) {
                            return XR_ERROR_VALIDATION_FAILURE;
                        }

                        projectionViewAllocator.push_back(
                            {proj->views[xr::StereoView::Left], proj->views[xr::StereoView::Right]});

                        for (uint32_t viewIndex = 0; viewIndex < xr::StereoView::Count; viewIndex++) {
                            for (uint32_t i = viewIndex; i < xr::QuadView::Count; i += xr::StereoView::Count) {
                                TraceLoggingWrite(
                                    g_traceProvider,
                                    "xrEndFrame_View",
                                    TLArg(i, "ViewIndex"),
                                    TLXArg(proj->views[i].subImage.swapchain, "Swapchain"),
                                    TLArg(proj->views[i].subImage.imageArrayIndex, "ImageArrayIndex"),
                                    TLArg(xr::ToString(proj->views[i].subImage.imageRect).c_str(), "ImageRect"),
                                    TLArg(xr::ToString(proj->views[i].pose).c_str(), "Pose"),
                                    TLArg(xr::ToString(proj->views[i].fov).c_str(), "Fov"));
                            }

                            const uint32_t focusViewIndex = viewIndex + xr::StereoView::Count;

                            std::unique_lock lock(m_swapchainsMutex);

                            const auto it = m_swapchains.find(proj->views[viewIndex].subImage.swapchain);
                            const auto it2 = m_swapchains.find(proj->views[focusViewIndex].subImage.swapchain);
                            if (it == m_swapchains.end() || it2 == m_swapchains.end()) {
                                return XR_ERROR_HANDLE_INVALID;
                            }

                            Swapchain& swapchainForStereoView = it->second;
                            Swapchain& swapchainForFocusView = it2->second;

                            // Allocate a destination swapchain.
                            if (swapchainForStereoView.fullFovSwapchain[viewIndex] == XR_NULL_HANDLE) {
                                XrSwapchainCreateInfo createInfo = swapchainForStereoView.createInfo;
                                createInfo.arraySize = 1;
                                createInfo.width = m_fullFovResolution.width;
                                createInfo.height = m_fullFovResolution.height;
                                CHECK_XRCMD(OpenXrApi::xrCreateSwapchain(
                                    session, &createInfo, &swapchainForStereoView.fullFovSwapchain[viewIndex]));
                            }

                            XrCompositionLayerProjectionView focusView = proj->views[focusViewIndex];
                            if (m_needFocusFovCorrectionQuirk) {
                                // Quirk for DCS World: the application does not pass the correct FOV for the
                                // focus views in xrEndFrame(). We must keep track of the correct values for
                                // each frame.
                                std::unique_lock lock(m_focusFovMutex);

                                const auto& cit = m_focusFovForDisplayTime.find(frameEndInfo->displayTime);
                                if (cit != m_focusFovForDisplayTime.cend()) {
                                    focusView.fov = focusViewIndex == xr::QuadView::FocusLeft ? cit->second.first
                                                                                              : cit->second.second;
                                }
                            }

                            // Composite the focus view and the stereo view together into a single stereo view.
                            compositeViewContent(viewIndex,
                                                 proj->views[viewIndex],
                                                 swapchainForStereoView,
                                                 focusView,
                                                 swapchainForFocusView,
                                                 proj->layerFlags);

                            // Patch the view to reference the new swapchain at full FOV.
                            XrCompositionLayerProjectionView& patchedView = projectionViewAllocator.back()[viewIndex];
                            patchedView.fov = m_cachedEyeFov[viewIndex];
                            patchedView.subImage.swapchain = swapchainForStereoView.fullFovSwapchain[viewIndex];
                            patchedView.subImage.imageArrayIndex = 0;
                            patchedView.subImage.imageRect.offset = {0, 0};
                            patchedView.subImage.imageRect.extent = m_fullFovResolution;
                        }

                        // Note: if a depth buffer was attached, we will use it as-is (per copy of the proj struct
                        // below, and therefore its entire chain of next structs). This is good: we will submit a depth
                        // that matches the composited view, but that is lower resolution.

                        projectionAllocator.push_back(*proj);
                        // Our shader always premultiplies the alpha channel.
                        projectionAllocator.back().layerFlags &= ~XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
                        projectionAllocator.back().views = projectionViewAllocator.back().data();
                        projectionAllocator.back().viewCount = xr::StereoView::Count;
                        layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&projectionAllocator.back()));

                    } else {
                        TraceLoggingWrite(g_traceProvider,
                                          "xrEndFrame_Layer",
                                          TLArg(xr::ToCString(frameEndInfo->layers[i]->type), "Type"));
                        layers.push_back(frameEndInfo->layers[i]);
                    }
                }

                chainFrameEndInfo.layers = layers.data();
                chainFrameEndInfo.layerCount = (uint32_t)layers.size();

                if (m_needFocusFovCorrectionQuirk) {
                    std::unique_lock lock(m_focusFovMutex);

                    // Delete all entries older than 1s.
                    while (!m_focusFovForDisplayTime.empty() &&
                           m_focusFovForDisplayTime.cbegin()->first < frameEndInfo->displayTime - 1'000'000'000) {
                        m_focusFovForDisplayTime.erase(m_focusFovForDisplayTime.begin());
                    }
                    TraceLoggingWrite(g_traceProvider,
                                      "xrEndFrame",
                                      TLArg(m_focusFovForDisplayTime.size(), "FovForDisplayTimeDictionarySize"));
                }
            }

            XrResult result = XR_ERROR_RUNTIME_FAILURE;
            {
                std::unique_lock lock(m_frameMutex);

                if (m_asyncWaitPromise.valid()) {
                    TraceLocalActivity(local);

                    // This is the latest point we must have fully waited a frame before proceeding.
                    //
                    // Note: we should not wait infinitely here, however certain patterns of engine calls may cause us
                    // to attempt a "double xrWaitFrame" when turning on Turbo. Use a timeout to detect that, and
                    // refrain from enqueing a second wait further down. This isn't a pretty solution, but it is simple
                    // and it seems to work effectively (minus the 1s freeze observed in-game).
                    TraceLoggingWriteStart(local, "AsyncWaitNow");
                    const auto ready = m_asyncWaitPromise.wait_for(1s) == std::future_status::ready;
                    TraceLoggingWriteStop(local, "AsyncWaitNow", TLArg(ready, "Ready"));
                    if (ready) {
                        m_asyncWaitPromise = {};
                    }

                    CHECK_XRCMD(OpenXrApi::xrBeginFrame(session, nullptr));
                }

                result = OpenXrApi::xrEndFrame(session, &chainFrameEndInfo);

                if (m_useTurboMode && !m_asyncWaitPromise.valid()) {
                    m_asyncWaitPolled = false;
                    m_asyncWaitCompleted = false;

                    // In Turbo mode, we kick off a wait thread immediately.
                    TraceLoggingWrite(g_traceProvider, "AsyncWaitStart");
                    m_asyncWaitPromise = std::async(std::launch::async, [&, session] {
                        TraceLocalActivity(local);

                        XrFrameState frameState{XR_TYPE_FRAME_STATE};
                        TraceLoggingWriteStart(local, "AsyncWaitFrame");
                        CHECK_XRCMD(OpenXrApi::xrWaitFrame(session, nullptr, &frameState));
                        TraceLoggingWriteStop(local,
                                              "AsyncWaitFrame",
                                              TLArg(frameState.predictedDisplayTime, "PredictedDisplayTime"),
                                              TLArg(frameState.predictedDisplayPeriod, "PredictedDisplayPeriod"));
                        {
                            std::unique_lock lock(m_asyncWaitMutex);

                            m_lastPredictedDisplayTime = frameState.predictedDisplayTime;
                            m_lastPredictedDisplayPeriod = frameState.predictedDisplayPeriod;

                            m_asyncWaitCompleted = true;
                        }
                    });
                }
            }

            return result;
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrCreateReferenceSpace
        XrResult xrCreateReferenceSpace(XrSession session,
                                        const XrReferenceSpaceCreateInfo* createInfo,
                                        XrSpace* space) override {
            if (createInfo->type != XR_TYPE_REFERENCE_SPACE_CREATE_INFO) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            TraceLoggingWrite(g_traceProvider,
                              "xrCreateReferenceSpace",
                              TLXArg(session, "Session"),
                              TLArg(xr::ToCString(createInfo->referenceSpaceType), "ReferenceSpaceType"),
                              TLArg(xr::ToString(createInfo->poseInReferenceSpace).c_str(), "PoseInReferenceSpace"));

            XrReferenceSpaceCreateInfo chainCreateInfo = *createInfo;

            if (createInfo->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_COMBINED_EYE_VARJO) {
                // Create a dummy space, we will keep track of those handles below.
                chainCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
            }

            const XrResult result = OpenXrApi::xrCreateReferenceSpace(session, &chainCreateInfo, space);

            if (XR_SUCCEEDED(result)) {
                TraceLoggingWrite(g_traceProvider, "xrCreateReferenceSpace", TLXArg(*space, "Space"));

                if (createInfo->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_COMBINED_EYE_VARJO) {
                    std::unique_lock lock(m_spacesMutex);

                    m_gazeSpaces.insert(*space);
                }
            }

            return result;
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrDestroySpace
        XrResult xrDestroySpace(XrSpace space) override {
            TraceLoggingWrite(g_traceProvider, "xrDestroySpace", TLXArg(space, "Space"));

            const XrResult result = OpenXrApi::xrDestroySpace(space);

            if (XR_SUCCEEDED(result)) {
                std::unique_lock lock(m_spacesMutex);

                m_gazeSpaces.erase(space);
            }

            return result;
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrLocateSpace
        XrResult xrLocateSpace(XrSpace space, XrSpace baseSpace, XrTime time, XrSpaceLocation* location) override {
            TraceLoggingWrite(g_traceProvider,
                              "xrLocateSpace",
                              TLXArg(space, "Space"),
                              TLXArg(baseSpace, "BaseSpace"),
                              TLArg(time, "Time"));

            std::unique_lock lock(m_spacesMutex);

            XrResult result = XR_ERROR_RUNTIME_FAILURE;
            if (m_gazeSpaces.count(space)) {
                if (location->type != XR_TYPE_SPACE_LOCATION) {
                    return XR_ERROR_VALIDATION_FAILURE;
                }

                if (time <= 0) {
                    return XR_ERROR_TIME_INVALID;
                }

                XrVector3f dummyVector{};
                if (getEyeGaze(time, true, dummyVector)) {
                    location->locationFlags = XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
                } else {
                    location->locationFlags = 0;
                }
                location->pose = Pose::Identity();

                result = XR_SUCCESS;
            } else {
                result = OpenXrApi::xrLocateSpace(space, baseSpace, time, location);
            }

            if (XR_SUCCEEDED(result)) {
                TraceLoggingWrite(g_traceProvider,
                                  "xrLocateSpace",
                                  TLArg(location->locationFlags, "LocationFlags"),
                                  TLArg(xr::ToString(location->pose).c_str(), "Pose"));
            }

            return result;
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrPollEvent
        XrResult xrPollEvent(XrInstance instance, XrEventDataBuffer* eventData) override {
            // TODO: Block/translate visibility mask events.
            return OpenXrApi::xrPollEvent(instance, eventData);
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrGetVisibilityMaskKHR
        XrResult xrGetVisibilityMaskKHR(XrSession session,
                                        XrViewConfigurationType viewConfigurationType,
                                        uint32_t viewIndex,
                                        XrVisibilityMaskTypeKHR visibilityMaskType,
                                        XrVisibilityMaskKHR* visibilityMask) override {
            if (visibilityMask->type != XR_TYPE_VISIBILITY_MASK_KHR) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            TraceLoggingWrite(g_traceProvider,
                              "xrGetVisibilityMaskKHR",
                              TLXArg(session, "Session"),
                              TLArg(xr::ToCString(viewConfigurationType), "ViewConfigurationType"),
                              TLArg(viewIndex, "ViewIndex"),
                              TLArg(xr::ToCString(visibilityMaskType), "VisibilityMaskType"),
                              TLArg(visibilityMask->vertexCapacityInput, "VertexCapacityInput"),
                              TLArg(visibilityMask->indexCapacityInput, "IndexCapacityInput"));

            // TODO: Conformance: viewConfigurationType must match the session.

            XrResult result = XR_ERROR_RUNTIME_FAILURE;
            if (viewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO &&
                viewIndex >= xr::StereoView::Count) {
                // No mask on the focus view.
                if (viewIndex == xr::QuadView::FocusLeft || viewIndex == xr::QuadView::FocusRight) {
                    visibilityMask->vertexCountOutput = 0;
                    visibilityMask->indexCountOutput = 0;

                    result = XR_SUCCESS;
                } else {
                    result = XR_ERROR_VALIDATION_FAILURE;
                }
            } else {
                // We will implement quad views on top of stereo. Use the regular mask for the peripheral view.
                if (viewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO) {
                    viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                }

                result = OpenXrApi::xrGetVisibilityMaskKHR(
                    session, viewConfigurationType, viewIndex, visibilityMaskType, visibilityMask);
            }

            return result;
        }

      private:
        struct Swapchain {
            std::deque<uint32_t> acquiredIndex;
            uint32_t lastReleasedIndex{0};

            XrSwapchainCreateInfo createInfo{};
            XrSwapchain fullFovSwapchain[xr::StereoView::Count]{XR_NULL_HANDLE, XR_NULL_HANDLE};
            ComPtr<ID3D11Texture2D> flatImage[xr::QuadView::Count];
            ComPtr<ID3D11Texture2D> sharpenedImage[xr::StereoView::Count];
        };

        bool getEyeGaze(XrTime time, bool getStateOnly, XrVector3f& unitVector) {
            if (!m_isEyeTrackingAvailable) {
                return false;
            }

            if (!m_debugSimulateTracking) {
                XrEyeGazesInfoFB eyeGazeInfo{XR_TYPE_EYE_GAZES_INFO_FB};
                eyeGazeInfo.baseSpace = m_viewSpace;
                eyeGazeInfo.time = time;

                XrEyeGazesFB eyeGaze{XR_TYPE_EYE_GAZES_FB};
                CHECK_XRCMD(OpenXrApi::xrGetEyeGazesFB(m_eyeTrackerFB, &eyeGazeInfo, &eyeGaze));

                if (!(eyeGaze.gaze[0].isValid && eyeGaze.gaze[1].isValid)) {
                    return false;
                }

                if (!(eyeGaze.gaze[0].gazeConfidence > 0.5f && eyeGaze.gaze[1].gazeConfidence > 0.5f)) {
                    return false;
                }

                if (!getStateOnly) {
                    // Average the poses from both eyes.
                    const auto gaze = LoadXrPose(Pose::Slerp(eyeGaze.gaze[0].gazePose, eyeGaze.gaze[1].gazePose, 0.5f));
                    const auto gazeProjectedPoint =
                        DirectX::XMVector3Transform(DirectX::XMVectorSet(0.f, 0.f, 1.f, 1.f), gaze);

                    unitVector.x = gazeProjectedPoint.m128_f32[0];
                    unitVector.y = -gazeProjectedPoint.m128_f32[1];
                    unitVector.z = gazeProjectedPoint.m128_f32[2];
                }
            } else {
                // Use the mouse to simulate eye tracking.
                RECT rect;
                rect.left = 1;
                rect.right = 999;
                rect.top = 1;
                rect.bottom = 999;
                ClipCursor(&rect);

                POINT cursor{};
                GetCursorPos(&cursor);

                XrVector2f point = {(float)cursor.x / 1000.f, (1000.f - cursor.y) / 1000.f};
                unitVector = Normalize({point.x - 0.5f, 0.5f - point.y, -0.35f});
            }

            TraceLoggingWrite(
                g_traceProvider, "xrLocateViews_EyeGaze", TLArg(xr::ToString(unitVector).c_str(), "GazeUnitVector"));

            return true;
        }

        void compositeViewContent(uint32_t viewIndex,
                                  const XrCompositionLayerProjectionView& stereoView,
                                  Swapchain& swapchainForStereoView,
                                  const XrCompositionLayerProjectionView& focusView,
                                  Swapchain& swapchainForFocusView,
                                  XrCompositionLayerFlags layerFlags) {
            // TODO: Support D3D12.

            // Grab the input textures.
            const auto getSourceImage = [&](const XrCompositionLayerProjectionView& view, Swapchain& swapchain) {
                uint32_t count;
                CHECK_XRCMD(OpenXrApi::xrEnumerateSwapchainImages(view.subImage.swapchain, 0, &count, nullptr));
                std::vector<XrSwapchainImageD3D11KHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
                CHECK_XRCMD(OpenXrApi::xrEnumerateSwapchainImages(
                    view.subImage.swapchain,
                    count,
                    &count,
                    reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())));
                return images[swapchain.lastReleasedIndex].texture;
            };
            ID3D11Texture2D* sourceImage = getSourceImage(stereoView, swapchainForStereoView);
            ID3D11Texture2D* sourceFocusImage = getSourceImage(focusView, swapchainForFocusView);

            // Grab the output texture.
            ID3D11Texture2D* destinationImage;
            {
                uint32_t acquiredImageIndex;
                CHECK_XRCMD(OpenXrApi::xrAcquireSwapchainImage(
                    swapchainForStereoView.fullFovSwapchain[viewIndex], nullptr, &acquiredImageIndex));
                XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                waitInfo.timeout = 10000000000;
                CHECK_XRCMD(
                    OpenXrApi::xrWaitSwapchainImage(swapchainForStereoView.fullFovSwapchain[viewIndex], &waitInfo));

                uint32_t count;
                CHECK_XRCMD(OpenXrApi::xrEnumerateSwapchainImages(
                    swapchainForStereoView.fullFovSwapchain[viewIndex], 0, &count, nullptr));
                std::vector<XrSwapchainImageD3D11KHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
                CHECK_XRCMD(OpenXrApi::xrEnumerateSwapchainImages(
                    swapchainForStereoView.fullFovSwapchain[viewIndex],
                    count,
                    &count,
                    reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())));
                destinationImage = images[acquiredImageIndex].texture;
            }

            // Grab a D3D context.
            {
                ComPtr<ID3D11Device> device;
                destinationImage->GetDevice(device.ReleaseAndGetAddressOf());

                if (!m_applicationDevice) {
                    initializeCompositionResources(device.Get());
                }
            }

            // Save the application context state.
            ComPtr<ID3DDeviceContextState> applicationContextState;
            m_renderContext->SwapDeviceContextState(m_layerContextState.Get(),
                                                    applicationContextState.ReleaseAndGetAddressOf());
            m_renderContext->ClearState();

            if (IsTraceEnabled()) {
                m_timerIndex = (m_timerIndex + 1) % std::size(m_compositionTimer);
                TraceLoggingWrite(g_traceProvider,
                                  "xrEndFrame_Perf",
                                  TLArg(m_compositionTimer[m_timerIndex]->query(), "CompositionTime"));
                m_compositionTimer[m_timerIndex]->start();
            }

            // Copy to a flat texture for sampling.
            const auto flattenSourceImage = [&](ID3D11Texture2D* image,
                                                const XrCompositionLayerProjectionView& view,
                                                Swapchain& swapchain,
                                                uint32_t startSlot) {
                D3D11_TEXTURE2D_DESC desc{};
                if (swapchain.flatImage[startSlot + viewIndex]) {
                    swapchain.flatImage[startSlot + viewIndex]->GetDesc(&desc);
                }
                if (!swapchain.flatImage[startSlot + viewIndex] || desc.Width != view.subImage.imageRect.extent.width ||
                    desc.Height != view.subImage.imageRect.extent.height) {
                    desc = {};
                    desc.ArraySize = 1;
                    desc.Width = view.subImage.imageRect.extent.width;
                    desc.Height = view.subImage.imageRect.extent.height;
                    desc.Format = (DXGI_FORMAT)swapchain.createInfo.format;
                    desc.MipLevels = 1;
                    desc.SampleDesc.Count = 1;
                    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    CHECK_HRCMD(m_applicationDevice->CreateTexture2D(
                        &desc, nullptr, swapchain.flatImage[startSlot + viewIndex].ReleaseAndGetAddressOf()));
                }
                D3D11_BOX box{};
                box.left = view.subImage.imageRect.offset.x;
                box.top = view.subImage.imageRect.offset.y;
                box.right = box.left + view.subImage.imageRect.extent.width;
                box.bottom = box.top + view.subImage.imageRect.extent.height;
                box.back = 1;
                m_renderContext->CopySubresourceRegion(swapchain.flatImage[startSlot + viewIndex].Get(),
                                                       0,
                                                       0,
                                                       0,
                                                       0,
                                                       image,
                                                       view.subImage.imageArrayIndex,
                                                       &box);
            };
            // TODO: We could reduce overhead by avoiding these 2 copies and modifying sampling in our shader to
            // consider the offset.
            flattenSourceImage(sourceImage, stereoView, swapchainForStereoView, 0);
            flattenSourceImage(sourceFocusImage, focusView, swapchainForFocusView, xr::StereoView::Count);

            // Sharpen if needed.
            if (m_sharpenFocusView) {
                {
                    D3D11_TEXTURE2D_DESC desc{};
                    if (swapchainForFocusView.sharpenedImage[viewIndex]) {
                        swapchainForFocusView.sharpenedImage[viewIndex]->GetDesc(&desc);
                    }
                    if (!swapchainForFocusView.sharpenedImage[viewIndex] ||
                        desc.Width != focusView.subImage.imageRect.extent.width ||
                        desc.Height != focusView.subImage.imageRect.extent.height) {
                        desc = {};
                        desc.ArraySize = 1;
                        desc.Width = focusView.subImage.imageRect.extent.width;
                        desc.Height = focusView.subImage.imageRect.extent.height;
                        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                        desc.MipLevels = 1;
                        desc.SampleDesc.Count = 1;
                        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
                        CHECK_HRCMD(m_applicationDevice->CreateTexture2D(
                            &desc, nullptr, swapchainForFocusView.sharpenedImage[viewIndex].ReleaseAndGetAddressOf()));
                    }
                }

                // Create ephemeral SRV/UAV.
                ComPtr<ID3D11ShaderResourceView> srv;
                {
                    D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
                    desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                    desc.Format = (DXGI_FORMAT)swapchainForFocusView.createInfo.format;
                    desc.Texture2D.MipLevels = 1;
                    CHECK_HRCMD(m_applicationDevice->CreateShaderResourceView(
                        swapchainForFocusView.flatImage[xr::StereoView::Count + viewIndex].Get(),
                        &desc,
                        srv.ReleaseAndGetAddressOf()));
                }
                ComPtr<ID3D11UnorderedAccessView> uav;
                {
                    D3D11_UNORDERED_ACCESS_VIEW_DESC desc{};
                    desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
                    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                    CHECK_HRCMD(m_applicationDevice->CreateUnorderedAccessView(
                        swapchainForFocusView.sharpenedImage[viewIndex].Get(), &desc, uav.ReleaseAndGetAddressOf()));
                }

                // Set up the shader.
                SharpeningCSConstants sharpening{};
                CasSetup(sharpening.Const0,
                         sharpening.Const1,
                         std::clamp(m_sharpenFocusView, 0.f, 1.f),
                         (AF1)focusView.subImage.imageRect.extent.width,
                         (AF1)focusView.subImage.imageRect.extent.height,
                         (AF1)focusView.subImage.imageRect.extent.width,
                         (AF1)focusView.subImage.imageRect.extent.height);
                {
                    D3D11_MAPPED_SUBRESOURCE mappedResources;
                    CHECK_HRCMD(m_renderContext->Map(
                        m_sharpeningCSConstants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResources));
                    memcpy(mappedResources.pData, &sharpening, sizeof(sharpening));
                    m_renderContext->Unmap(m_sharpeningCSConstants.Get(), 0);
                }

                m_renderContext->CSSetConstantBuffers(0, 1, m_sharpeningCSConstants.GetAddressOf());
                m_renderContext->CSSetShaderResources(0, 1, srv.GetAddressOf());
                m_renderContext->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);
                m_renderContext->CSSetShader(m_sharpeningCS.Get(), nullptr, 0);

                // This value is the image region dim that each thread group of the CAS shader operates on
                static const int threadGroupWorkRegionDim = 16;
                int dispatchX = (focusView.subImage.imageRect.extent.width + (threadGroupWorkRegionDim - 1)) /
                                threadGroupWorkRegionDim;
                int dispatchY = (focusView.subImage.imageRect.extent.height + (threadGroupWorkRegionDim - 1)) /
                                threadGroupWorkRegionDim;
                m_renderContext->Dispatch((UINT)dispatchX, (UINT)dispatchY, 1);

                // Unbind the resources used below to avoid D3D validation errors.
                ID3D11UnorderedAccessView* nullUAV[] = {nullptr};
                m_renderContext->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
            }

            // Create ephemeral SRV/RTV.
            ComPtr<ID3D11ShaderResourceView> srvForStereoView;
            {
                D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
                desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                desc.Format = (DXGI_FORMAT)swapchainForStereoView.createInfo.format;
                desc.Texture2D.MipLevels = 1;
                CHECK_HRCMD(
                    m_applicationDevice->CreateShaderResourceView(swapchainForStereoView.flatImage[viewIndex].Get(),
                                                                  &desc,
                                                                  srvForStereoView.ReleaseAndGetAddressOf()));
            }
            ComPtr<ID3D11ShaderResourceView> srvForFocusView;
            {
                D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
                desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                desc.Format = m_sharpenFocusView ? DXGI_FORMAT_R16G16B16A16_FLOAT
                                                 : (DXGI_FORMAT)swapchainForFocusView.createInfo.format;
                desc.Texture2D.MipLevels = 1;
                CHECK_HRCMD(m_applicationDevice->CreateShaderResourceView(
                    m_sharpenFocusView ? swapchainForFocusView.sharpenedImage[viewIndex].Get()
                                       : swapchainForFocusView.flatImage[xr::StereoView::Count + viewIndex].Get(),
                    &desc,
                    srvForFocusView.ReleaseAndGetAddressOf()));
            }
            ComPtr<ID3D11RenderTargetView> rtv;
            {
                D3D11_RENDER_TARGET_VIEW_DESC desc{};
                desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
                desc.Format = (DXGI_FORMAT)swapchainForStereoView.createInfo.format;
                CHECK_HRCMD(
                    m_applicationDevice->CreateRenderTargetView(destinationImage, &desc, rtv.ReleaseAndGetAddressOf()));
            }

            // Compute the projection.
            ProjectionVSConstants projection;
            {
                const DirectX::XMMATRIX baseLayerViewProjection =
                    ComposeProjectionMatrix(m_cachedEyeFov[viewIndex], NearFar{0.1f, 20.f});
                const DirectX::XMMATRIX layerViewProjection =
                    ComposeProjectionMatrix(focusView.fov, NearFar{0.1f, 20.f});

                DirectX::XMStoreFloat4x4(
                    &projection.focusProjection,
                    DirectX::XMMatrixTranspose(DirectX::XMMatrixInverse(nullptr, baseLayerViewProjection) *
                                               layerViewProjection));
            }
            {
                D3D11_MAPPED_SUBRESOURCE mappedResources;
                CHECK_HRCMD(m_renderContext->Map(
                    m_projectionVSConstants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResources));
                memcpy(mappedResources.pData, &projection, sizeof(projection));
                m_renderContext->Unmap(m_projectionVSConstants.Get(), 0);
            }

            ProjectionPSConstants drawing{};
            drawing.smoothingArea = m_smoothenFocusViewEdges;
            drawing.ignoreAlpha = ~(layerFlags & XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT);
            drawing.isUnpremultipliedAlpha = layerFlags & XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
            drawing.debugFocusView = m_debugFocusView;
            {
                D3D11_MAPPED_SUBRESOURCE mappedResources;
                CHECK_HRCMD(m_renderContext->Map(
                    m_projectionPSConstants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResources));
                memcpy(mappedResources.pData, &drawing, sizeof(drawing));
                m_renderContext->Unmap(m_projectionPSConstants.Get(), 0);
            }

            // Dispatch the composition shader.
            m_renderContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
            m_renderContext->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
            m_renderContext->RSSetState(m_noDepthRasterizer.Get());
            D3D11_VIEWPORT viewport{};
            viewport.Width = (float)m_fullFovResolution.width;
            viewport.Height = (float)m_fullFovResolution.height;
            viewport.MaxDepth = 1.f;
            m_renderContext->RSSetViewports(1, &viewport);
            m_renderContext->VSSetConstantBuffers(0, 1, m_projectionVSConstants.GetAddressOf());
            m_renderContext->VSSetShader(m_projectionVS.Get(), nullptr, 0);
            m_renderContext->PSSetConstantBuffers(0, 1, m_projectionPSConstants.GetAddressOf());
            m_renderContext->PSSetSamplers(0, 1, m_linearClampSampler.GetAddressOf());
            ID3D11ShaderResourceView* srvs[] = {srvForStereoView.Get(), srvForFocusView.Get()};
            m_renderContext->PSSetShaderResources(0, 2, srvs);
            m_renderContext->PSSetShader(m_projectionPS.Get(), nullptr, 0);
            m_renderContext->Draw(3, 0);

            if (IsTraceEnabled()) {
                m_compositionTimer[m_timerIndex]->stop();
            }

            // Restore the application context state.
            m_renderContext->SwapDeviceContextState(applicationContextState.Get(), nullptr);

            CHECK_XRCMD(
                OpenXrApi::xrReleaseSwapchainImage(swapchainForStereoView.fullFovSwapchain[viewIndex], nullptr));
        }

        void initializeCompositionResources(ID3D11Device* device) {
            {
                UINT creationFlags = 0;
                if (device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED) {
                    creationFlags |= D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED;
                }
                const D3D_FEATURE_LEVEL featureLevel = device->GetFeatureLevel();

                CHECK_HRCMD(device->QueryInterface(m_applicationDevice.ReleaseAndGetAddressOf()));

                // Create a switchable context state for the API layer.
                CHECK_HRCMD(
                    m_applicationDevice->CreateDeviceContextState(creationFlags,
                                                                  &featureLevel,
                                                                  1,
                                                                  D3D11_SDK_VERSION,
                                                                  __uuidof(ID3D11Device),
                                                                  nullptr,
                                                                  m_layerContextState.ReleaseAndGetAddressOf()));
            }

            // For FOV projection.
            {
                ComPtr<ID3D11DeviceContext> context;
                m_applicationDevice->GetImmediateContext(context.ReleaseAndGetAddressOf());
                CHECK_HRCMD(context->QueryInterface(m_renderContext.ReleaseAndGetAddressOf()));
            }
            {
                D3D11_SAMPLER_DESC desc{};
                desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
                desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
                desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
                desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
                desc.MaxAnisotropy = 1;
                desc.MinLOD = D3D11_MIP_LOD_BIAS_MIN;
                desc.MaxLOD = D3D11_MIP_LOD_BIAS_MAX;
                CHECK_HRCMD(
                    m_applicationDevice->CreateSamplerState(&desc, m_linearClampSampler.ReleaseAndGetAddressOf()));
            }
            {
                D3D11_RASTERIZER_DESC desc{};
                desc.FillMode = D3D11_FILL_SOLID;
                desc.CullMode = D3D11_CULL_NONE;
                desc.FrontCounterClockwise = TRUE;
                CHECK_HRCMD(
                    m_applicationDevice->CreateRasterizerState(&desc, m_noDepthRasterizer.ReleaseAndGetAddressOf()));
            }
            {
                D3D11_BUFFER_DESC desc{};
                desc.ByteWidth = (UINT)std::max((size_t)16, sizeof(ProjectionVSConstants));
                desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                desc.Usage = D3D11_USAGE_DYNAMIC;
                desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                CHECK_HRCMD(m_applicationDevice->CreateBuffer(
                    &desc, nullptr, m_projectionVSConstants.ReleaseAndGetAddressOf()));
            }
            {
                D3D11_BUFFER_DESC desc{};
                desc.ByteWidth = (UINT)std::max((size_t)16, sizeof(ProjectionPSConstants));
                desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                desc.Usage = D3D11_USAGE_DYNAMIC;
                desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                CHECK_HRCMD(m_applicationDevice->CreateBuffer(
                    &desc, nullptr, m_projectionPSConstants.ReleaseAndGetAddressOf()));
            }
            CHECK_HRCMD(m_applicationDevice->CreateVertexShader(
                g_ProjectionVS, sizeof(g_ProjectionVS), nullptr, m_projectionVS.ReleaseAndGetAddressOf()));
            CHECK_HRCMD(m_applicationDevice->CreatePixelShader(
                g_ProjectionPS, sizeof(g_ProjectionPS), nullptr, m_projectionPS.ReleaseAndGetAddressOf()));

            // For CAS sharpening.
            {
                D3D11_BUFFER_DESC desc{};
                desc.ByteWidth = (UINT)std::max((size_t)16, sizeof(SharpeningCSConstants));
                desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                desc.Usage = D3D11_USAGE_DYNAMIC;
                desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                CHECK_HRCMD(m_applicationDevice->CreateBuffer(
                    &desc, nullptr, m_sharpeningCSConstants.ReleaseAndGetAddressOf()));
            }
            CHECK_HRCMD(m_applicationDevice->CreateComputeShader(
                g_SharpeningCS, sizeof(g_SharpeningCS), nullptr, m_sharpeningCS.ReleaseAndGetAddressOf()));

            // For statistics.
            {
                XrGraphicsBindingD3D11KHR bindings{};
                bindings.device = device;
                std::shared_ptr<graphics::IGraphicsDevice> graphicsDevice =
                    graphics::internal::wrapApplicationDevice(bindings);
                for (uint32_t i = 0; i < std::size(m_compositionTimer); i++) {
                    m_compositionTimer[i] = graphicsDevice->createTimer();
                }
            }
        }

        void populateFovTables(XrSystemId systemId, XrSession session = XR_NULL_HANDLE) {
            if (!m_needComputeBaseFov) {
                return;
            }

            cacheStereoView(systemId, session);

            XrView view[xr::StereoView::Count]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
            XrVector2f projectedGaze[xr::StereoView::Count]{{}, {}};
            for (uint32_t eye = 0; eye < xr::StereoView::Count; eye++) {
                view[eye].fov = m_cachedEyeFov[eye];
                view[eye].pose = m_cachedEyePoses[eye];

                // Calculate the "resting" gaze position.
                ProjectPoint(view[eye], {0.f, 0.f, -1.f}, projectedGaze[eye]);
                m_centerOfFov[eye].x = (projectedGaze[eye].x + 1.f) / 2.f;
                m_centerOfFov[eye].y = (1.f - projectedGaze[eye].y + m_verticalFocusBias) / 2.f;
            }

            for (uint32_t foveated = 0; foveated <= 1; foveated++) {
                for (uint32_t eye = 0; eye < xr::StereoView::Count; eye++) {
                    const uint32_t viewIndex = 2 + (foveated * 2) + eye;

                    // Apply the FOV multiplier.
                    std::tie(m_cachedEyeFov[viewIndex].angleLeft, m_cachedEyeFov[viewIndex].angleRight) =
                        Fov::Scale(std::make_pair(m_cachedEyeFov[eye].angleLeft, m_cachedEyeFov[eye].angleRight),
                                   m_horizontalFovSection[foveated]);
                    std::tie(m_cachedEyeFov[viewIndex].angleDown, m_cachedEyeFov[viewIndex].angleUp) =
                        Fov::Scale(std::make_pair(m_cachedEyeFov[eye].angleDown, m_cachedEyeFov[eye].angleUp),
                                   m_verticalFovSection[foveated]);

                    // Adjust for (fixed) gaze.
                    std::tie(m_cachedEyeFov[viewIndex].angleLeft, m_cachedEyeFov[viewIndex].angleRight) = Fov::Lerp(
                        std::make_pair(m_cachedEyeFov[eye].angleLeft, m_cachedEyeFov[eye].angleRight),
                        std::make_pair(m_cachedEyeFov[viewIndex].angleLeft, m_cachedEyeFov[viewIndex].angleRight),
                        m_centerOfFov[eye].x);
                    std::tie(m_cachedEyeFov[viewIndex].angleDown, m_cachedEyeFov[viewIndex].angleUp) = Fov::Lerp(
                        std::make_pair(m_cachedEyeFov[eye].angleDown, m_cachedEyeFov[eye].angleUp),
                        std::make_pair(m_cachedEyeFov[viewIndex].angleDown, m_cachedEyeFov[viewIndex].angleUp),
                        m_centerOfFov[eye].y);
                }
            }

            {
                XrViewConfigurationView stereoViews[xr::StereoView::Count]{{XR_TYPE_VIEW_CONFIGURATION_VIEW},
                                                                           {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
                uint32_t count;
                CHECK_XRCMD(OpenXrApi::xrEnumerateViewConfigurationViews(GetXrInstance(),
                                                                         systemId,
                                                                         XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                                         xr::StereoView::Count,
                                                                         &count,
                                                                         stereoViews));
                const float newWidth =
                    m_focusPixelDensity * stereoViews[xr::StereoView::Left].recommendedImageRectWidth;
                const float ratio = (float)stereoViews[xr::StereoView::Left].recommendedImageRectHeight /
                                    stereoViews[xr::StereoView::Left].recommendedImageRectWidth;
                const float newHeight = newWidth * ratio;

                m_fullFovResolution.width =
                    std::min((uint32_t)newWidth, stereoViews[xr::StereoView::Left].maxImageRectWidth);
                m_fullFovResolution.height =
                    std::min((uint32_t)newHeight, stereoViews[xr::StereoView::Left].maxImageRectHeight);
            }

            m_needComputeBaseFov = false;
        }

        void cacheStereoView(XrSystemId systemId, XrSession session = XR_NULL_HANDLE) {
            bool needCleanupSession = false;
            if (session == XR_NULL_HANDLE) {
                // Create an ephemeral session to query the information we need.
                XrGraphicsRequirementsD3D11KHR graphicsRequirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
                CHECK_XRCMD(
                    OpenXrApi::xrGetD3D11GraphicsRequirementsKHR(GetXrInstance(), systemId, &graphicsRequirements));

                std::shared_ptr<graphics::IGraphicsDevice> graphicsDevice =
                    graphics::internal::createD3D11CompositionDevice(graphicsRequirements.adapterLuid);

                XrGraphicsBindingD3D11KHR graphicsBindings{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
                graphicsBindings.device = graphicsDevice->getNativeDevice<graphics::D3D11>();

                XrSessionCreateInfo sessionCreateInfo{XR_TYPE_SESSION_CREATE_INFO};
                sessionCreateInfo.systemId = systemId;
                sessionCreateInfo.next = &graphicsBindings;

                CHECK_XRCMD(OpenXrApi::xrCreateSession(GetXrInstance(), &sessionCreateInfo, &session));

                // Wait for the session to be ready.
                {
                    while (true) {
                        XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
                        const XrResult result = OpenXrApi::xrPollEvent(GetXrInstance(), &event);
                        if (result == XR_SUCCESS) {
                            if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                                const XrEventDataSessionStateChanged& sessionEvent =
                                    *reinterpret_cast<XrEventDataSessionStateChanged*>(&event);

                                if (sessionEvent.state == XR_SESSION_STATE_READY) {
                                    break;
                                }
                            }
                        }
                        CHECK_XRCMD(result);

                        // TODO: Need some sort of timeout.
                        if (result == XR_EVENT_UNAVAILABLE) {
                            std::this_thread::sleep_for(100ms);
                        }
                    }
                }

                XrSessionBeginInfo beginSessionInfo{XR_TYPE_SESSION_BEGIN_INFO};
                beginSessionInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                CHECK_XRCMD(OpenXrApi::xrBeginSession(session, &beginSessionInfo));

                needCleanupSession = true;
            }

            XrReferenceSpaceCreateInfo spaceCreateInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
            spaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
            spaceCreateInfo.poseInReferenceSpace = Pose::Identity();

            XrSpace viewSpace;
            CHECK_XRCMD(OpenXrApi::xrCreateReferenceSpace(session, &spaceCreateInfo, &viewSpace));

            XrViewLocateInfo viewLocateInfo{XR_TYPE_VIEW_LOCATE_INFO};
            viewLocateInfo.space = viewSpace;
            viewLocateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;

            XrView view[xr::StereoView::Count]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
            while (true) {
                XrFrameState frameState{XR_TYPE_FRAME_STATE};
                CHECK_XRCMD(OpenXrApi::xrWaitFrame(session, nullptr, &frameState));
                CHECK_XRCMD(OpenXrApi::xrBeginFrame(session, nullptr));

                viewLocateInfo.displayTime = frameState.predictedDisplayTime;

                XrViewState viewState{XR_TYPE_VIEW_STATE};
                uint32_t count;
                CHECK_XRCMD(OpenXrApi::xrLocateViews(session, &viewLocateInfo, &viewState, 2, &count, view));
                if (viewState.viewStateFlags &
                    (XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
                    break;
                }
            }

            if (needCleanupSession) {
                CHECK_XRCMD(OpenXrApi::xrDestroySession(session));

                // Purge events for this session.
                // TODO: This might steal legitimate events from the runtime.
                {
                    while (true) {
                        XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
                        const XrResult result = OpenXrApi::xrPollEvent(GetXrInstance(), &event);
                        if (result == XR_EVENT_UNAVAILABLE) {
                            break;
                        }
                    }
                }
            }

            for (uint32_t eye = 0; eye < xr::StereoView::Count; eye++) {
                m_cachedEyeFov[eye] = view[eye].fov;
                m_cachedEyePoses[eye] = view[eye].pose;
            }
        }

        void handleDebugKeys() {
            if (m_debugKeys) {
                bool log = false;

#define DEBUG_ACTION(label, key, action)                                                                               \
    static bool wasCtrl##label##Pressed = false;                                                                       \
    const bool isCtrl##label##Pressed = GetAsyncKeyState(VK_CONTROL) < 0 && GetAsyncKeyState(key) < 0;                 \
    if (!wasCtrl##label##Pressed && isCtrl##label##Pressed) {                                                          \
        log = true;                                                                                                    \
        action;                                                                                                        \
    }                                                                                                                  \
    wasCtrl##label##Pressed = isCtrl##label##Pressed;

                DEBUG_ACTION(
                    SharpenLess, 'J', { m_sharpenFocusView = std::clamp(m_sharpenFocusView - 0.1f, 0.f, 1.f); });
                DEBUG_ACTION(
                    SharpenMore, 'U', { m_sharpenFocusView = std::clamp(m_sharpenFocusView + 0.1f, 0.f, 1.f); });
                DEBUG_ACTION(ToggleSharpen, 'N', {
                    static float lastSharpenFocusView = m_sharpenFocusView;
                    if (m_sharpenFocusView) {
                        lastSharpenFocusView = m_sharpenFocusView;
                        m_sharpenFocusView = 0;
                    } else {
                        m_sharpenFocusView = lastSharpenFocusView;
                    }
                });
                DEBUG_ACTION(SmoothenThicknessLess, 'I', {
                    m_smoothenFocusViewEdges = std::clamp(m_smoothenFocusViewEdges - 0.01f, 0.f, 1.f);
                });
                DEBUG_ACTION(SmoothenThicknessMore, 'K', {
                    m_smoothenFocusViewEdges = std::clamp(m_smoothenFocusViewEdges + 0.01f, 0.f, 1.f);
                });
                DEBUG_ACTION(ToggleSmoothen, 'M', {
                    static float lastSmoothenFocusViewEdges = m_smoothenFocusViewEdges;
                    if (m_smoothenFocusViewEdges) {
                        lastSmoothenFocusViewEdges = m_smoothenFocusViewEdges;
                        m_smoothenFocusViewEdges = 0;
                    } else {
                        m_smoothenFocusViewEdges = lastSmoothenFocusViewEdges;
                    }
                });
                DEBUG_ACTION(VerticalFocusBiasUp, 'O', {
                    m_verticalFocusBias = std::clamp(m_verticalFocusBias + 0.01f, -1.f, 1.f);
                });
                DEBUG_ACTION(VerticalFocusBiasDown, 'L', {
                    m_verticalFocusBias = std::clamp(m_verticalFocusBias - 0.01f, -1.f, 1.f);
                });

                if (log) {
                    Log(fmt::format("sharpen_focus_view={:.1f}\n", m_sharpenFocusView));
                    Log(fmt::format("smoothen_focus_view_edges={:.2f}\n", m_smoothenFocusViewEdges));
                    Log(fmt::format("vertical_focus_bias={:.2f}\n", m_verticalFocusBias));
                }
            }
        }

        void LoadConfiguration() {
            std::ifstream configFile;

            // Look in %LocalAppData% first, then fallback to your installation folder.
            auto configPath = localAppData / "settings.cfg";
            Log(fmt::format("Trying to locate configuration file at '{}'...\n", configPath.string()));
            configFile.open(configPath);
            if (!configFile.is_open()) {
                Log("Not found\n");
                configPath = dllHome / "settings.cfg";
                Log(fmt::format("Trying to locate configuration file at '{}'...\n", configPath.string()));
                configFile.open(configPath);
            }

            if (configFile.is_open()) {
                unsigned int lineNumber = 0;
                std::string line;
                while (std::getline(configFile, line)) {
                    lineNumber++;
                    ParseConfigurationStatement(line, lineNumber);
                }
                configFile.close();
            } else {
                Log("No configuration was found\n");
            }
        }

        void ParseConfigurationStatement(const std::string& line, unsigned int lineNumber) {
            try {
                const auto offset = line.find('=');
                if (offset != std::string::npos) {
                    const std::string name = line.substr(0, offset);
                    const std::string value = line.substr(offset + 1);

                    bool parsed = false;
                    if (name == "peripheral_multiplier") {
                        m_peripheralPixelDensity = std::max(0.1f, std::stof(value));
                        parsed = true;
                    } else if (name == "focus_multiplier") {
                        m_focusPixelDensity = std::max(0.1f, std::stof(value));
                        parsed = true;
                    } else if (name == "horizontal_fixed_section") {
                        m_horizontalFovSection[0] = std::clamp(std::stof(value), 0.1f, 0.9f);
                        parsed = true;
                    } else if (name == "vertical_fixed_section") {
                        m_verticalFovSection[0] = std::clamp(std::stof(value), 0.1f, 0.9f);
                        parsed = true;
                    } else if (name == "horizontal_focus_section") {
                        m_horizontalFovSection[1] = std::clamp(std::stof(value), 0.1f, 0.9f);
                        parsed = true;
                    } else if (name == "vertical_focus_section") {
                        m_verticalFovSection[1] = std::clamp(std::stof(value), 0.1f, 0.9f);
                        parsed = true;
                    } else if (name == "vertical_focus_bias") {
                        m_verticalFocusBias = std::clamp(std::stof(value), -0.5f, 0.5f);
                        parsed = true;
                    } else if (name == "prefer_foveated_rendering") {
                        m_preferFoveatedRendering = std::stoi(value);
                        parsed = true;
                    } else if (name == "smoothen_focus_view_edges") {
                        m_smoothenFocusViewEdges = std::clamp(std::stof(value), 0.f, 0.5f);
                        parsed = true;
                    } else if (name == "sharpen_focus_view") {
                        m_sharpenFocusView = std::clamp(std::stof(value), 0.f, 1.f);
                        parsed = true;
                    } else if (name == "turbo_mode") {
                        m_useTurboMode = std::stoi(value);
                        parsed = true;
                    } else if (name == "debug_simulate_tracking") {
                        m_debugSimulateTracking = std::stoi(value);
                        parsed = true;
                    } else if (name == "debug_focus_view") {
                        m_debugFocusView = std::stoi(value);
                        parsed = true;
                    } else if (name == "debug_force_no_foveated") {
                        m_debugForceNoFoveated = std::stoi(value);
                        parsed = true;
                    } else if (name == "debug_defer_populate_fov_table") {
                        m_debugDeferPopulateFovTable = std::stoi(value);
                        parsed = true;
                    } else if (name == "debug_keys") {
                        m_debugKeys = std::stoi(value);
                        parsed = true;
                    } else {
                        Log("L%u: Unrecognized option\n", lineNumber);
                    }

                    if (parsed) {
                        Log(fmt::format("  Found option '{}={}'\n", name, value));
                    }
                } else {
                    Log("L%u: Improperly formatted option\n", lineNumber);
                }
            } catch (...) {
                Log("L%u: Parsing error\n", lineNumber);
            }
        }

        bool m_bypassApiLayer{false};
        bool m_useQuadViews{false};
        bool m_requestedFoveatedRendering{false};
        bool m_loggedResolution{false};
        bool m_isEyeTrackingAvailable{false};

        XrSystemId m_systemId{XR_NULL_SYSTEM_ID};

        float m_peripheralPixelDensity{0.4f};
        float m_focusPixelDensity{1.1f};
        // [0] = non-foveated, [1] = foveated
        float m_horizontalFovSection[2]{0.75f, 0.5f};
        float m_verticalFovSection[2]{0.7f, 0.5f};
        float m_verticalFocusBias{0.25f};
        bool m_preferFoveatedRendering{true};
        float m_smoothenFocusViewEdges{0.2f};
        float m_sharpenFocusView{0.7f};
        bool m_useTurboMode{true};

        bool m_needComputeBaseFov{true};
        // [0] = left, [1] = right
        // [2] = left focus non-foveated, [3] = right focus non-foveated,
        // [4] = left focus foveated, [5] = right focus foveated
        XrFovf m_cachedEyeFov[xr::QuadView::Count + 2]{};
        XrPosef m_cachedEyePoses[xr::StereoView::Count]{};
        XrVector2f m_centerOfFov[xr::StereoView::Count]{};

        XrExtent2Di m_fullFovResolution{};

        std::mutex m_swapchainsMutex;
        std::unordered_map<XrSwapchain, Swapchain> m_swapchains;

        std::mutex m_spacesMutex;
        std::set<XrSpace> m_gazeSpaces;

        XrEyeTrackerFB m_eyeTrackerFB{XR_NULL_HANDLE};
        XrSpace m_viewSpace{XR_NULL_HANDLE};

        ComPtr<ID3D11Device5> m_applicationDevice;
        ComPtr<ID3D11DeviceContext4> m_renderContext;
        ComPtr<ID3DDeviceContextState> m_layerContextState;
        ComPtr<ID3D11SamplerState> m_linearClampSampler;
        ComPtr<ID3D11RasterizerState> m_noDepthRasterizer;
        ComPtr<ID3D11Buffer> m_projectionVSConstants;
        ComPtr<ID3D11Buffer> m_projectionPSConstants;
        ComPtr<ID3D11VertexShader> m_projectionVS;
        ComPtr<ID3D11PixelShader> m_projectionPS;
        ComPtr<ID3D11Buffer> m_sharpeningCSConstants;
        ComPtr<ID3D11ComputeShader> m_sharpeningCS;

        // Turbo mode.
        std::chrono::time_point<std::chrono::steady_clock> m_lastFrameWaitTimestamp{};
        std::mutex m_frameMutex;
        XrTime m_waitedFrameTime;
        std::mutex m_asyncWaitMutex;
        std::future<void> m_asyncWaitPromise;
        XrTime m_lastPredictedDisplayTime{0};
        XrTime m_lastPredictedDisplayPeriod{0};
        bool m_asyncWaitPolled{false};
        bool m_asyncWaitCompleted{false};

        // FOV submission quirk.
        bool m_needFocusFovCorrectionQuirk{false};
        std::mutex m_focusFovMutex;
        std::map<XrTime, std::pair<XrFovf, XrFovf>> m_focusFovForDisplayTime;

        bool m_isSupportedGraphicsApi{false};

        bool m_debugFocusView{false};
        bool m_debugSimulateTracking{false};
        bool m_debugForceNoFoveated{false};
        bool m_debugDeferPopulateFovTable{false};
        bool m_debugKeys{false};

        std::shared_ptr<graphics::IGraphicsTimer> m_compositionTimer[3];
        uint32_t m_timerIndex{0};
    };

    // This method is required by the framework to instantiate your OpenXrApi implementation.
    OpenXrApi* GetInstance() {
        if (!g_instance) {
            g_instance = std::make_unique<OpenXrLayer>();
        }
        return g_instance.get();
    }

} // namespace openxr_api_layer

namespace openxr_api_layer::log {
    extern std::ofstream logStream;
}

template <typename TMethod>
void DetourDllAttach(const char* dll, const char* target, TMethod hooked, TMethod& original) {
    if (original) {
        // Already hooked.
        return;
    }

    HMODULE handle;
    CHECK_MSG(GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_PIN, dll, &handle), "Failed to get DLL handle");

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    original = (TMethod)GetProcAddress(handle, target);
    CHECK_MSG(original, "Failed to resolve symbol");
    DetourAttach((PVOID*)&original, hooked);

    CHECK_MSG(DetourTransactionCommit() == NO_ERROR, "Detour failed");
}

bool (*original_session_InitializeSession)() = nullptr;
bool WINAPI hooked_session_InitializeSession() {
    openxr_api_layer::log::Log("--> session_InitializeSession()\n");
    if (original_session_InitializeSession()) {
        HMODULE unityOpenXRModule;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, "UnityOpenXR.dll", &unityOpenXRModule) &&
            unityOpenXRModule) {
            bool (*pfnRequestEnableExtensionString)(const char* extensionString);
            pfnRequestEnableExtensionString = reinterpret_cast<decltype(pfnRequestEnableExtensionString)>(
                GetProcAddress(unityOpenXRModule, "unity_ext_RequestEnableExtensionString"));
            if (pfnRequestEnableExtensionString) {
                openxr_api_layer::log::Log("Found Unity extension activation callback\n");
                openxr_api_layer::log::Log("Result = %d\n",
                                           pfnRequestEnableExtensionString(XR_VARJO_QUAD_VIEWS_EXTENSION_NAME));
            }
        }
        openxr_api_layer::log::Log("<-- session_InitializeSession() = true\n");
        return true;
    }
    openxr_api_layer::log::Log("<-- session_InitializeSession() = false\n");
    return false;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        DetourRestoreAfterWith();

        TraceLoggingRegister(openxr_api_layer::log::g_traceProvider);
        // XXX: Unity hack.
        {
            HMODULE unityOpenXRModule = LoadLibrary(L"UnityOpenXR.dll");
            if (unityOpenXRModule) {
                DetourDllAttach("UnityOpenXR.dll",
                                "session_InitializeSession",
                                hooked_session_InitializeSession,
                                original_session_InitializeSession);
            }
        }
        break;

    case DLL_PROCESS_DETACH:
        TraceLoggingUnregister(openxr_api_layer::log::g_traceProvider);
        break;

    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
        break;
    }
    return TRUE;
}
