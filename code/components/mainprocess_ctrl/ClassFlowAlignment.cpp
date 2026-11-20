#include "ClassFlowAlignment.h"
#include "../../include/defines.h"

#include <nvs.h>
#include <esp_log.h>

#include "CImage.h"
#include "CImageMod.h"
#include "CImageTplMatch.h"
#include "ClassControlCamera.h"
#include "ClassLogFile.h"
#include "MainFlowControl.h"
#include "helper.h"


static const char *TAG = "ALIGN";


ClassFlowAlignment::ClassFlowAlignment()
{
    presetFlowStateHandler(true);
}


bool ClassFlowAlignment::loadParameter()
{
    cfgDataPtr = &ConfigClass::getInstance()->get()->sectionImageAlignment;

    if (cfgDataPtr == NULL) {
        LogFile.writeToFile(ESP_LOG_ERROR, TAG, "Invalid config");
        return false;
    }

    // Configure two alignment marker
    if (cfgDataPtr->alignmentAlgo == ALIGNALGO_ROTATE_AND_ALIGN_SAD_1CH ||
        cfgDataPtr->alignmentAlgo == ALIGNALGO_ROTATE_AND_ALIGN_SAD_3CH) {
        for (int i = 0; i < 2; i++) {
            int x = 0, y = 0, channels = 0;
            std::string sIndex = std::to_string(i + 1);
            alignmentMarker[i].markerImageFilename = "/sdcard/config/marker" + sIndex + ".jpg";

            // Check availability of marker image
            if (!fileExists(alignmentMarker[i].markerImageFilename)) {
                LogFile.writeToFile(ESP_LOG_ERROR, TAG,
                                    "Marker missing: > Create alignment marker | File: " + alignmentMarker[i].markerImageFilename);
                return false;
            }

            // Parse marker image meta data (dimension, channels)
            if (!stbi_info(alignmentMarker[i].markerImageFilename.c_str(), &x, &y, &channels)) {
                std::string failureReason = stbi_failure_reason() ? stbi_failure_reason() : "Unknown";
                LogFile.writeToFile(ESP_LOG_ERROR, TAG,
                                    "Marker invalid > Recreate alignment marker | File: " + alignmentMarker[i].markerImageFilename +
                                        " | Reason: " + failureReason);
                return false;
            }

            // Create BMP image container (with required image dimension)
            alignmentMarker[i].markerImage = new CImage("marker" + sIndex, x, y, channels, true);
            if (!alignmentMarker[i].markerImage) {
                LogFile.writeToFile(ESP_LOG_ERROR, TAG, "Failed to create image container");
                return false;
            }

            // Read marker to BMP image container
            if (alignmentMarker[i].markerImage->loadJpgFromFile(alignmentMarker[i].markerImageFilename.c_str(), true) != ESP_OK) {
                return false;
            }

            // Preset marker parameter
            alignmentMarker[i].alignmentAlgo = cfgDataPtr->alignmentAlgo;
            alignmentMarker[i].searchX = cfgDataPtr->searchField.x;
            alignmentMarker[i].searchY = cfgDataPtr->searchField.y;
            alignmentMarker[i].targetX = cfgDataPtr->marker[i].x;
            alignmentMarker[i].targetY = cfgDataPtr->marker[i].y;
            alignmentMarker[i].width = alignmentMarker[i].markerImage->getWidth();
            alignmentMarker[i].height = alignmentMarker[i].markerImage->getHeight();

            // ROI position plausibility check
            int imgWidth = 640;
            int imgHeight = 480;
            cameraCtrl.getOutputFrameSize(imgWidth, imgHeight);

            if (alignmentMarker[i].targetX < 1 || (alignmentMarker[i].targetX > (imgWidth - 1 - alignmentMarker[i].width))) {
                LogFile.writeToFile(ESP_LOG_ERROR, TAG, "One or more alignment marker out of image area (x). Check alignment marker");
                return false;
            }

            if (alignmentMarker[i].targetY < 1 || (alignmentMarker[i].targetY > (imgHeight - 1 - alignmentMarker[i].height))) {
                LogFile.writeToFile(ESP_LOG_ERROR, TAG, "One or more alignment marker out of image area (y). Check alignment marker");
                return false;
            }
        }
    }

    if (cfgDataPtr->displayValidation.enabled) {
        if (cfgDataPtr->displayValidation.roi.empty()) {
            LogFile.writeToFile(ESP_LOG_ERROR, TAG, "Display validation enabled but no validation regions are configured");
            return false;
        }

        const std::string referencePath = "/sdcard/config/reference.jpg";
        CImage referenceImage("displayValidationReference", referencePath);
        if (!referenceImage.isValid()) {
            LogFile.writeToFile(ESP_LOG_ERROR, TAG, "Display validation reference image missing or invalid: " + referencePath);
            return false;
        }

        int imageWidth = 640;
        int imageHeight = 480;
        cameraCtrl.getOutputFrameSize(imageWidth, imageHeight);
        if (referenceImage.getWidth() != imageWidth || referenceImage.getHeight() != imageHeight) {
            LogFile.writeToFile(ESP_LOG_ERROR, TAG, "Display validation reference image dimensions do not match camera output");
            return false;
        }

        for (size_t i = 0; i < cfgDataPtr->displayValidation.roi.size(); ++i) {
            const auto &roi = cfgDataPtr->displayValidation.roi[i];
            if (roi.x < 0 || roi.y < 0 || roi.dx < 1 || roi.dy < 1 || roi.x >= imageWidth || roi.y >= imageHeight ||
                roi.dx > imageWidth - roi.x || roi.dy > imageHeight - roi.y) {
                LogFile.writeToFile(ESP_LOG_ERROR, TAG, "Display validation region " + std::to_string(i + 1) +
                                                            " is outside the reference image");
                return false;
            }

            AlignmentMarker marker{};
            marker.targetX = roi.x;
            marker.targetY = roi.y;
            marker.width = roi.dx;
            marker.height = roi.dy;
            marker.searchX = 2;
            marker.searchY = 2;
            marker.markerImage =
                new CImage("displayValidationRoi" + std::to_string(i + 1), roi.dx, roi.dy, referenceImage.getChannels(), true);
            if (!marker.markerImage || !marker.markerImage->isValid()) {
                delete marker.markerImage;
                LogFile.writeToFile(ESP_LOG_ERROR, TAG, "Failed to allocate display validation template");
                return false;
            }
            displayValidationMarkers.push_back(marker);
            if (CImageMod::crop(referenceImage, roi.x, roi.y, roi.dx, roi.dy, *displayValidationMarkers.back().markerImage) != ESP_OK) {
                LogFile.writeToFile(ESP_LOG_ERROR, TAG, "Failed to crop display validation template");
                return false;
            }
        }
    }

    return true;
}


bool ClassFlowAlignment::doFlow(std::string time)
{
    presetFlowStateHandler(false, time);
    if (!flowImageData->imgProcess) {
        LogFile.writeToFile(ESP_LOG_ERROR, TAG, "Missing process image");
        setFlowStateHandlerEvent(-3);
        return false;
    }

    CImage imgAlgRoi(*flowImageData->imgProcess);
    if (!imgAlgRoi.isValid()) {
        LogFile.writeToFile(ESP_LOG_ERROR, TAG, "Failed to create imgAlgRoi");
        setFlowStateHandlerEvent(-3);
        return false;
    }

    imgAlgRoi.setName("imgAlgRoi");

    // 1. Perform basic rotation
    // *******************************************
    float rotation = cfgDataPtr->imageRotation;
    if (rotation != 0.0f && cfgDataPtr->alignmentAlgo != ALIGNALGO_OFF) {
        CImageMod::rotate(*flowImageData->imgProcess, rotation, imgAlgRoi, true);

        if (cfgDataPtr->debug.saveAllFiles) {
            flowImageData->imgProcess->saveJpgToFile(formatFileName("/sdcard/img_tmp/rot.jpg"));
        }
    }

    LogFile.writeToFile(ESP_LOG_DEBUG, TAG, "Initial rotation: " + to_stringWithPrecision(rotation, 1));

    // 2. Perform alignment algorithm (template match)
    // Note: Only if any additional alignment algo is configured
    // *******************************************
    if (cfgDataPtr->alignmentAlgo == ALIGNALGO_ROTATE_AND_ALIGN_SAD_1CH ||
        cfgDataPtr->alignmentAlgo == ALIGNALGO_ROTATE_AND_ALIGN_SAD_3CH) {
        TplMatchStatus alignRetval = CImageTplMatch::invokeTplMatch(*flowImageData->imgProcess, imgAlgRoi, alignmentMarker[0],
                                                                    alignmentMarker[1]);
        if (alignRetval != TPL_MATCH_OK) { // Alignment not successful
            LogFile.writeToFile(ESP_LOG_ERROR, TAG,
                                "Fine alignment unsuccessful. Use alignment marker areas with sharp edges, unique shapes and high contrast "
                                "on a sharply focused image");
            setFlowStateHandlerEvent(-1); // Set error event code for post cycle error handler 'doPostProcessEventHandling'
        }

        drawAlignmentMarker(imgAlgRoi);
    }

    if (getFlowState()->isSuccessful && cfgDataPtr->displayValidation.enabled && !validateDisplay(*flowImageData->imgProcess)) {
        setFlowStateHandlerEvent(-2);
    }

    if (getFlowState()->isSuccessful) {
        flowctrl.drawDigitRoi(imgAlgRoi);
        flowctrl.drawAnalogRoi(imgAlgRoi);
    }

    // 3. Save aligned image with overlays
    // *******************************************
    imgAlgRoi.saveJpgToContainer(flowImageData->imgVisu);

    if (cfgDataPtr->debug.saveAllFiles) {
        flowImageData->imgVisu->saveJpgToFile(formatFileName("/sdcard/img_tmp/alg_roi.jpg"));
    }

    if (!getFlowState()->isSuccessful) {
        return false;
    }

    return true;
}


bool ClassFlowAlignment::validateDisplay(CImage &image)
{
    const float threshold = static_cast<float>(cfgDataPtr->displayValidation.similarityThreshold);
    for (size_t i = 0; i < displayValidationMarkers.size(); ++i) {
        float similarity = 0.0f;
        const TplMatchStatus status = CImageTplMatch::compareTemplate(image, displayValidationMarkers[i], similarity);
        if (status != TPL_MATCH_OK) {
            LogFile.writeToFile(ESP_LOG_ERROR, TAG, "Display validation comparison failed for region " + std::to_string(i + 1));
            return false;
        }
        LogFile.writeToFile(ESP_LOG_DEBUG, TAG, "Display validation region " + std::to_string(i + 1) +
                                                   " similarity: " + to_stringWithPrecision(similarity, 1) + "%");
        if (similarity < threshold) {
            LogFile.writeToFile(ESP_LOG_WARN, TAG,
                                "Display validation rejected image: region " + std::to_string(i + 1) + " similarity " +
                                    to_stringWithPrecision(similarity, 1) + "% below threshold " +
                                    to_stringWithPrecision(threshold, 1) + "%");
            return false;
        }
    }
    return true;
}


void ClassFlowAlignment::doPostProcessEventHandling()
{
    // Post cycle process handling can be included here. Function is called after processing cycle is completed
    for (int i = 0; i < getFlowState()->EventCode.size(); i++) {
        if (cfgDataPtr->debug.saveDebugInfo && getFlowState()->EventCode[i] == -1) { // If saving error logs enabled and alignment failed
                                                                                     // event
            time_t actualtime;
            time(&actualtime);

            // Define path, e.g. /sdcard/log/debug/20230814/20230814-125528/ClassFlowAlignment
            std::string destination = std::string(LOG_DEBUG_ROOT_FOLDER) + "/" +
                                      getFlowState()->ExecutionTime.DEFAULT_TIME_FORMAT_DATE_EXTR + "/" + getFlowState()->ExecutionTime +
                                      "/" + getFlowState()->ClassName;

            if (!makeDir(destination)) {
                return;
            }

            // Save algo results in file
            std::string resultFileName = "/alignment_failed.txt";
            FILE *fpResult = fopen((destination + resultFileName).c_str(), "w");
            fwrite(alignmentMarker[0].errorMsg.c_str(), (alignmentMarker[0].errorMsg).length(), 1, fpResult);
            fclose(fpResult);

            // Draw alignment marker and save image
            drawAlignmentMarker(*flowImageData->imgProcess);
            flowImageData->imgProcess->saveJpgToFile(formatFileName(destination + "/alg_misalign.jpg"));

            LogFile.writeToFile(ESP_LOG_DEBUG, TAG, "Fine alignment unsuccessful, debug infos saved: " + destination);
        }
    }
}


void ClassFlowAlignment::drawAlignmentMarker(CImage &image)
{
    if (!image.isValid()) {
        LogFile.writeToFile(ESP_LOG_ERROR, TAG, "drawAlignmentMarker: Invalid image");
        return;
    }

    CImageMod::drawRect(image, alignmentMarker[0].targetX, alignmentMarker[0].targetY, alignmentMarker[0].width, alignmentMarker[0].height,
                        255, 51, 51, 2);
    CImageMod::drawRect(image, alignmentMarker[1].targetX, alignmentMarker[1].targetY, alignmentMarker[1].width, alignmentMarker[1].height,
                        255, 51, 51, 2);
}


ClassFlowAlignment::~ClassFlowAlignment()
{
    for (int i = 0; i < 2; ++i) {
        delete alignmentMarker[i].markerImage;
    }
    for (auto &marker : displayValidationMarkers) {
        delete marker.markerImage;
    }
}
