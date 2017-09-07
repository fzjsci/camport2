#include "../common/common.hpp"

static char buffer[1024 * 1024];
static int  n;
static volatile bool exit_main;


struct CallbackData {
    int             index;
    TY_DEV_HANDLE   hDevice;
    DepthRender*    render;
};

void frameHandler(TY_FRAME_DATA* frame, void* userdata) {
    CallbackData* pData = (CallbackData*) userdata;
    LOGD("=== Get frame %d", ++pData->index);

    cv::Mat depth, irl, irr, color;
    parseFrame(*frame, &depth, &irl, &irr, &color, 0);
    if(!depth.empty()){
        cv::Mat colorDepth = pData->render->Compute(depth);
        cv::imshow("ColorDepth", colorDepth);
    }
    if(!irl.empty()){ cv::imshow("LeftIR", irl); }
    if(!irr.empty()){ cv::imshow("RightIR", irr); }
    if(!color.empty()){ cv::imshow("Color", color); }

    int key = cv::waitKey(1);
    switch(key & 0xff) {
    case 0xff:
        break;
    case 'q':
        exit_main = true;
        break;
    default:
        LOGD("Unmapped key %d", key);
    }

    LOGD("=== Callback: Re-enqueue buffer(%p, %d)", frame->userBuffer, frame->bufferSize);
    ASSERT_OK( TYEnqueueBuffer(pData->hDevice, frame->userBuffer, frame->bufferSize) );
}

int main(int argc, char* argv[]) {
    const char* IP = NULL;
    TY_DEV_HANDLE hDevice;

    for(int i = 1; i < argc; i++) {
        if(strcmp(argv[i], "-ip") == 0) {
            IP = argv[++i];
        } else if(strcmp(argv[i], "-h") == 0) {
            LOGI("Usage: SimpleView_Callback [-h] [-ip <IP>]");
            return 0;
        }
    }

    LOGD("=== Init lib");
    ASSERT_OK( TYInitLib() );
    TY_VERSION_INFO* pVer = (TY_VERSION_INFO*)buffer;
    ASSERT_OK( TYLibVersion(pVer) );
    LOGD("     - lib version: %d.%d.%d", pVer->major, pVer->minor, pVer->patch);

    if(IP) {
        LOGD("=== Open device %s", IP);
        ASSERT_OK( TYOpenDeviceWithIP(IP, &hDevice) );
    } else {
        LOGD("=== Get device info");
        ASSERT_OK( TYGetDeviceNumber(&n) );
        LOGD("     - device number %d", n);

        TY_DEVICE_BASE_INFO* pBaseInfo = (TY_DEVICE_BASE_INFO*)buffer;
        ASSERT_OK( TYGetDeviceList(pBaseInfo, 100, &n) );

        if(n == 0) {
            LOGD("=== No device got");
            return -1;
        }

        LOGD("=== Open device 0");
        ASSERT_OK( TYOpenDevice(pBaseInfo[0].id, &hDevice) );
    }


    int32_t allComps;
    ASSERT_OK( TYGetComponentIDs(hDevice, &allComps) );
    if(allComps & TY_COMPONENT_RGB_CAM) {
        LOGD("=== Has RGB camera, open RGB cam");
        ASSERT_OK( TYEnableComponents(hDevice, TY_COMPONENT_RGB_CAM) );
    }

    LOGD("=== Configure components, open depth cam");
    int32_t componentIDs = TY_COMPONENT_IR_CAM_LEFT | TY_COMPONENT_IR_CAM_RIGHT;
    //int32_t componentIDs = TY_COMPONENT_RGB_CAM | TY_COMPONENT_IR_CAM_LEFT;
    ASSERT_OK( TYEnableComponents(hDevice, componentIDs) );

    LOGD("=== Configure feature, set resolution to 640x480.");
    LOGD("Note: DM460 resolution feature is in component TY_COMPONENT_DEVICE,");
    LOGD("      other device may lays in some other components.");
    TY_FEATURE_INFO info;
    TY_STATUS ty_status = TYGetFeatureInfo(hDevice, TY_COMPONENT_DEPTH_CAM, TY_ENUM_IMAGE_MODE, &info);
    if ((info.accessMode & TY_ACCESS_WRITABLE) && (ty_status == TY_STATUS_OK)) {
        int err = TYSetEnum(hDevice, TY_COMPONENT_DEPTH_CAM, TY_ENUM_IMAGE_MODE, TY_IMAGE_MODE_640x480);
        ASSERT(err == TY_STATUS_OK || err == TY_STATUS_NOT_PERMITTED);
    }

    int err = TYSetEnum(hDevice, TY_COMPONENT_RGB_CAM, TY_ENUM_IMAGE_MODE, TY_IMAGE_MODE_640x480);
    ASSERT(err == TY_STATUS_OK || err == TY_STATUS_NOT_PERMITTED);

    LOGD("=== Prepare image buffer");
    int32_t frameSize;
    ASSERT_OK( TYGetFrameBufferSize(hDevice, &frameSize) );
    LOGD("     - Get size of framebuffer, %d", frameSize);
    ASSERT( frameSize >= 640 * 480 * 2 );

    LOGD("     - Allocate & enqueue buffers");
    char* frameBuffer[2];
    frameBuffer[0] = new char[frameSize];
    frameBuffer[1] = new char[frameSize];
    LOGD("     - Enqueue buffer (%p, %d)", frameBuffer[0], frameSize);
    ASSERT_OK( TYEnqueueBuffer(hDevice, frameBuffer[0], frameSize) );
    LOGD("     - Enqueue buffer (%p, %d)", frameBuffer[1], frameSize);
    ASSERT_OK( TYEnqueueBuffer(hDevice, frameBuffer[1], frameSize) );

    LOGD("=== Register callback");
    LOGD("Note: Callback may block internal data receiving,");
    LOGD("      so that user should not do long time work in callback.");
    LOGD("      To avoid copying data, we pop the framebuffer from buffer queue and");
    LOGD("      give it back to user, user should call TYEnqueueBuffer to re-enqueue it.");
    DepthRender render;
    CallbackData cb_data;
    cb_data.index = 0;
    cb_data.hDevice = hDevice;
    cb_data.render = &render;
    // ASSERT_OK( TYRegisterCallback(hDevice, frameHandler, &cb_data) );

    LOGD("=== Disable trigger mode");
    ty_status = TYGetFeatureInfo(hDevice, TY_COMPONENT_DEVICE, TY_BOOL_TRIGGER_MODE, &info);
    if ((info.accessMode & TY_ACCESS_WRITABLE) && (ty_status == TY_STATUS_OK)) {
        ASSERT_OK(TYSetBool(hDevice, TY_COMPONENT_DEVICE, TY_BOOL_TRIGGER_MODE, false));
    }

    LOGD("=== Start capture");
    ASSERT_OK( TYStartCapture(hDevice) );

    LOGD("=== While loop to fetch frame");
    exit_main = false;
    TY_FRAME_DATA frame;

    while(!exit_main) {
        int err = TYFetchFrame(hDevice, &frame, -1);
        if( err != TY_STATUS_OK ) {
            LOGD("... Drop one frame");
        } else {
            frameHandler(&frame, &cb_data);
        }
    }

    ASSERT_OK( TYStopCapture(hDevice) );
    ASSERT_OK( TYCloseDevice(hDevice) );
    ASSERT_OK( TYDeinitLib() );
    // MSLEEP(10); // sleep to ensure buffer is not used any more
    delete frameBuffer[0];
    delete frameBuffer[1];

    LOGD("=== Main done!");
    return 0;
}
