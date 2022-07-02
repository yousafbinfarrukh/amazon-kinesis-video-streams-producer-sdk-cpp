#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <string.h>
#include <chrono>
#include <Logger.h>
#include "KinesisVideoProducer.h"
#include <vector>
#include <stdlib.h>
#include <mutex>
#include <IotCertCredentialProvider.h>
#include <unistd.h> 
	

#include <com/amazonaws/kinesis/video/cproducer/Include.h>
#include <aws/core/Aws.h>
#include <aws/monitoring/CloudWatchClient.h>
#include <aws/monitoring/model/PutMetricDataRequest.h>
#include <aws/logs/CloudWatchLogsClient.h>
#include <aws/logs/model/CreateLogGroupRequest.h>
#include <aws/logs/model/CreateLogStreamRequest.h>
#include <aws/logs/model/PutLogEventsRequest.h>
#include <aws/logs/model/DeleteLogStreamRequest.h>
#include <aws/logs/model/DescribeLogStreamsRequest.h>

using namespace std;
using namespace std::chrono;
using namespace com::amazonaws::kinesis::video;
using namespace log4cplus;

#ifdef __cplusplus
extern "C" {
#endif

int gstreamer_init(int, char **);

#ifdef __cplusplus
}
#endif

LOGGER_TAG("com.amazonaws.kinesis.video.gstreamer");

#define DEFAULT_RETENTION_PERIOD_HOURS 2
#define DEFAULT_KMS_KEY_ID ""
#define DEFAULT_STREAMING_TYPE STREAMING_TYPE_REALTIME
#define DEFAULT_CONTENT_TYPE "video/h264"
#define DEFAULT_MAX_LATENCY_SECONDS 60
#define DEFAULT_FRAGMENT_DURATION_MILLISECONDS 2000
#define DEFAULT_TIMECODE_SCALE_MILLISECONDS 1
#define DEFAULT_KEY_FRAME_FRAGMENTATION TRUE
#define DEFAULT_FRAME_TIMECODES TRUE
#define DEFAULT_ABSOLUTE_FRAGMENT_TIMES TRUE
#define DEFAULT_FRAGMENT_ACKS TRUE
#define DEFAULT_RESTART_ON_ERROR TRUE
#define DEFAULT_RECALCULATE_METRICS TRUE
#define DEFAULT_STREAM_FRAMERATE 25
#define DEFAULT_AVG_BANDWIDTH_BPS (4 * 1024 * 1024)
#define DEFAULT_BUFFER_DURATION_SECONDS 120
#define DEFAULT_REPLAY_DURATION_SECONDS 40
#define DEFAULT_CONNECTION_STALENESS_SECONDS 60
#define DEFAULT_CODEC_ID "V_MPEG4/ISO/AVC"
#define DEFAULT_TRACKNAME "kinesis_video"
#define DEFAULT_FRAME_DURATION_MS 1
#define DEFAULT_CREDENTIAL_ROTATION_SECONDS 3600
#define DEFAULT_CREDENTIAL_EXPIRATION_SECONDS 180


Aws::CloudWatch::Model::Dimension DIMENSION_PER_STREAM;


int TESTING_FPS = 25;



typedef enum _StreamSource {
    TEST_SOURCE,
    FILE_SOURCE,
    LIVE_SOURCE,
    RTSP_SOURCE
} StreamSource;

typedef struct _CanaryConfig
{
    _CanaryConfig():
            streamName("DefaultStreamName"),
            sourceType("TEST_SOURCE"),
            canaryRunType("NORMAL"),
            streamType("REALTIME"), 
            canaryLabel("DEFAULT_CANARY_LABEL"), // need to decide on a default value
            cpUrl("DEFAULT_CPURL"), // need to decide on a default value
            fragmentSize(DEFAULT_FRAGMENT_DURATION_MILLISECONDS),
            canaryDuration(20), // [seconds]
            bufferDuration(DEFAULT_BUFFER_DURATION_SECONDS),
            storageSizeInBytes(0)
            {}

    string streamName;
    string sourceType;
    string canaryRunType; // normal/continuous or intermitent
    string streamType; // real-time or offline
    string canaryLabel;
    string cpUrl;
    int fragmentSize; // [milliseconds]
    int canaryDuration;
    int bufferDuration; // [seconds]
    int storageSizeInBytes;
    // IoT credential stuff
} CanaryConfig;

typedef struct _CustomData {
    _CustomData(CanaryConfig canaryConfig):
            calcSleepTimeOffset(false),
            sleepTimeStamp(0),
            sleepTimeOffset(0),
            totalPutFrameErrorCount(0),
            totalErrorAckCount(0),
            lastKeyFrameTime(0),
            curKeyFrameTime(0),
            onFirstFrame(true),
            streamSource(TEST_SOURCE),
            h264_stream_supported(false),
            synthetic_dts(0),
            last_unpersisted_file_idx(0),
            stream_status(STATUS_SUCCESS),
            base_pts(0),
            max_frame_pts(0),
            key_frame_pts(0),
            main_loop(NULL),
            first_pts(GST_CLOCK_TIME_NONE),
            use_absolute_fragment_times(true) {
        producer_start_time = chrono::duration_cast<nanoseconds>(systemCurrentTime().time_since_epoch()).count(); // [nanoSeconds]
        client_config.region = "us-west-2";
        pCWclient = nullptr;
        timeOfNextKeyFrame = new map<uint64_t, uint64_t>();
        timeCounter = producer_start_time / 1000000000; // [seconds]
        // Default first intermittent run to 1 min
        runTill = producer_start_time / 1000000000 / 60 + 1; // [minutes]
        pCanaryConfig = &canaryConfig;
    }
    CanaryConfig *pCanaryConfig;

    Aws::Client::ClientConfiguration client_config;
    bool onFirstFrame;

    Aws::CloudWatch::CloudWatchClient *pCWclient;

    int runTill;
    int sleepTimeOffset;
    int sleepTimeStamp;
    bool calcSleepTimeOffset;

    GMainLoop *main_loop;
    unique_ptr<KinesisVideoProducer> kinesis_video_producer;
    shared_ptr<KinesisVideoStream> kinesis_video_stream;
    bool stream_started;
    bool h264_stream_supported;
    char *stream_name;
    mutex file_list_mtx;

    map<uint64_t, uint64_t>* timeOfNextKeyFrame;
    uint64_t lastKeyFrameTime;
    uint64_t curKeyFrameTime;

    double timeCounter;
    double totalPutFrameErrorCount;
    double totalErrorAckCount;

    // list of files to upload.
    // index of file in file_list that application is currently trying to upload.

    // index of last file in file_list that haven't been persisted.
    atomic_uint last_unpersisted_file_idx;

    // stores any error status code reported by StreamErrorCallback.
    atomic_uint stream_status;

    // Since each file's timestamp start at 0, need to add all subsequent file's timestamp to base_pts starting from the
    // second file to avoid fragment overlapping. When starting a new putMedia session, this should be set to 0.
    // Unit: ns
    uint64_t base_pts;

    // Max pts in a file. This will be added to the base_pts for the next file. When starting a new putMedia session,
    // this should be set to 0.
    // Unit: ns
    uint64_t max_frame_pts;

    // When uploading file, store the pts of frames that has flag FRAME_FLAG_KEY_FRAME. When the entire file has been uploaded,
    // key_frame_pts contains the timetamp of the last fragment in the file. key_frame_pts is then stored into last_fragment_ts
    // of the file.
    // Unit: ns
    uint64_t key_frame_pts;

    // Used in file uploading only. Assuming frame timestamp are relative. Add producer_start_time to each frame's
    // timestamp to convert them to absolute timestamp. This way fragments dont overlap after token rotation when doing
    // file uploading.
    uint64_t producer_start_time; // [nanoSeconds]

    volatile StreamSource streamSource;

    string rtsp_url;

    unique_ptr<Credentials> credential;

    uint64_t synthetic_dts;

    bool use_absolute_fragment_times;

    // Pts of first video frame
    uint64_t first_pts;
} CustomData;

namespace com { namespace amazonaws { namespace kinesis { namespace video {

class SampleClientCallbackProvider : public ClientCallbackProvider {
public:

    UINT64 getCallbackCustomData() override {
        return reinterpret_cast<UINT64> (this);
    }

    StorageOverflowPressureFunc getStorageOverflowPressureCallback() override {
        return storageOverflowPressure;
    }

    static STATUS storageOverflowPressure(UINT64 custom_handle, UINT64 remaining_bytes);
};

class SampleStreamCallbackProvider : public StreamCallbackProvider {
    UINT64 custom_data_;
public:
    SampleStreamCallbackProvider(UINT64 custom_data) : custom_data_(custom_data) {}

    UINT64 getCallbackCustomData() override {
        return custom_data_;
    }

    StreamConnectionStaleFunc getStreamConnectionStaleCallback() override {
        return streamConnectionStaleHandler;
    };

    StreamErrorReportFunc getStreamErrorReportCallback() override {
        return streamErrorReportHandler;
    };

    DroppedFrameReportFunc getDroppedFrameReportCallback() override {
        return droppedFrameReportHandler;
    };

    FragmentAckReceivedFunc getFragmentAckReceivedCallback() override {
        return fragmentAckReceivedHandler;
    };

private:
    static STATUS
    streamConnectionStaleHandler(UINT64 custom_data, STREAM_HANDLE stream_handle,
                                 UINT64 last_buffering_ack);

    static STATUS
    streamErrorReportHandler(UINT64 custom_data, STREAM_HANDLE stream_handle, UPLOAD_HANDLE upload_handle, UINT64 errored_timecode,
                             STATUS status_code);

    static STATUS
    droppedFrameReportHandler(UINT64 custom_data, STREAM_HANDLE stream_handle,
                              UINT64 dropped_frame_timecode);

    static STATUS
    fragmentAckReceivedHandler( UINT64 custom_data, STREAM_HANDLE stream_handle,
                                UPLOAD_HANDLE upload_handle, PFragmentAck pFragmentAck);
};

class SampleCredentialProvider : public StaticCredentialProvider {
    // Test rotation period is 40 second for the grace period.
    const std::chrono::duration<uint64_t> ROTATION_PERIOD = std::chrono::seconds(DEFAULT_CREDENTIAL_ROTATION_SECONDS);
public:
    SampleCredentialProvider(const Credentials &credentials) :
            StaticCredentialProvider(credentials) {}

    void updateCredentials(Credentials &credentials) override {
        // Copy the stored creds forward
        credentials = credentials_;

        // Update only the expiration
        auto now_time = std::chrono::duration_cast<std::chrono::seconds>(
                systemCurrentTime().time_since_epoch());
        auto expiration_seconds = now_time + ROTATION_PERIOD;
        credentials.setExpiration(std::chrono::seconds(expiration_seconds.count()));
        LOG_INFO("New credentials expiration is " << credentials.getExpiration().count());
    }
};

class SampleDeviceInfoProvider : public DefaultDeviceInfoProvider {
public:
    device_info_t getDeviceInfo() override {
        auto device_info = DefaultDeviceInfoProvider::getDeviceInfo();
        // Set the storage size to 128mb
        device_info.storageInfo.storageSize = 128 * 1024 * 1024;
        return device_info;
    }
};

STATUS
SampleClientCallbackProvider::storageOverflowPressure(UINT64 custom_handle, UINT64 remaining_bytes) {
    UNUSED_PARAM(custom_handle);
    LOG_WARN("Reporting storage overflow. Bytes remaining " << remaining_bytes);
    return STATUS_SUCCESS;
}

STATUS SampleStreamCallbackProvider::streamConnectionStaleHandler(UINT64 custom_data,
                                                                  STREAM_HANDLE stream_handle,
                                                                  UINT64 last_buffering_ack) {
    LOG_WARN("Reporting stream stale. Last ACK received " << last_buffering_ack);
    return STATUS_SUCCESS;
}

STATUS
SampleStreamCallbackProvider::streamErrorReportHandler(UINT64 custom_data, STREAM_HANDLE stream_handle,
                                                       UPLOAD_HANDLE upload_handle, UINT64 errored_timecode, STATUS status_code) {
    LOG_ERROR("Reporting stream error. Errored timecode: " << errored_timecode << " Status: "
                                                           << status_code);
    CustomData *data = reinterpret_cast<CustomData *>(custom_data);
    bool terminate_pipeline = false;

    // Terminate pipeline if error is not retriable or if error is retriable but we are streaming file.
    // When streaming file, we choose to terminate the pipeline on error because the easiest way to recover
    // is to stream the file from the beginning again.
    // In realtime streaming, retriable error can be handled underneath. Otherwise terminate pipeline
    // and store error status if error is fatal.
    if ((IS_RETRIABLE_ERROR(status_code) && data->streamSource == FILE_SOURCE) ||
        (!IS_RETRIABLE_ERROR(status_code) && !IS_RECOVERABLE_ERROR(status_code))) {
        data->stream_status = status_code;
        terminate_pipeline = true;
    }

    if (terminate_pipeline && data->main_loop != NULL) {
        LOG_WARN("Terminating pipeline due to unrecoverable stream error: " << status_code);
        g_main_loop_quit(data->main_loop);
    }

    return STATUS_SUCCESS;
}

STATUS
SampleStreamCallbackProvider::droppedFrameReportHandler(UINT64 custom_data, STREAM_HANDLE stream_handle,
                                                        UINT64 dropped_frame_timecode) {
    LOG_WARN("Reporting dropped frame. Frame timecode " << dropped_frame_timecode);
    return STATUS_SUCCESS;
}

STATUS
SampleStreamCallbackProvider::fragmentAckReceivedHandler(UINT64 custom_data, STREAM_HANDLE stream_handle,
                                                         UPLOAD_HANDLE upload_handle, PFragmentAck pFragmentAck) {
    CustomData *data = reinterpret_cast<CustomData *>(custom_data);
    // std::unique_lock<std::mutex> lk(data->file_list_mtx);
    uint64_t timeOfFragmentEndSent = data->timeOfNextKeyFrame->find(pFragmentAck->timestamp)->second / 10000;
    
    // When Canary sleeps, timeOfFragmentEndSent become less than currentTimeStamp, don't send that one
    if (timeOfFragmentEndSent > pFragmentAck->timestamp)
    {
        if (pFragmentAck->ackType == FRAGMENT_ACK_TYPE_PERSISTED)
        {
            Aws::CloudWatch::Model::MetricDatum persistedAckLatency_datum;
            Aws::CloudWatch::Model::PutMetricDataRequest cwRequest;
            cwRequest.SetNamespace("KinesisVideoSDKCanaryCPP");

            auto currentTimestamp = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
            auto persistedAckLatency = currentTimestamp - timeOfFragmentEndSent ;//- data->sleepTimeOffset; // [milliseconds]
            cout << "currentTimestamp: " << currentTimestamp << endl;
            cout << "timeOfFragmentEndSent: " << timeOfFragmentEndSent << endl;
            cout << "sleeptimeOffset: " << data->sleepTimeOffset << endl;
            cout << "persistedAckLatency: " << persistedAckLatency << endl;
            persistedAckLatency_datum.SetMetricName("PersistedAckLatency");
            persistedAckLatency_datum.AddDimensions(DIMENSION_PER_STREAM);
            persistedAckLatency_datum.SetValue(persistedAckLatency);
            persistedAckLatency_datum.SetUnit(Aws::CloudWatch::Model::StandardUnit::Milliseconds);
            cwRequest.AddMetricData(persistedAckLatency_datum);

            auto outcome = data->pCWclient->PutMetricData(cwRequest);
            if (!outcome.IsSuccess())
            {
                std::cout << "Failed to put PersistedAckLatency metric data:" <<
                    outcome.GetError().GetMessage() << std::endl;
            }
            else
            {
                std::cout << "Successfully put PersistedAckLatency metric data" << std::endl;
            }
        } else if (pFragmentAck->ackType == FRAGMENT_ACK_TYPE_RECEIVED)
            {
                Aws::CloudWatch::Model::MetricDatum recievedAckLatency_datum;
                Aws::CloudWatch::Model::PutMetricDataRequest cwRequest;
                cwRequest.SetNamespace("KinesisVideoSDKCanaryCPP");

                auto currentTimestamp = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
                auto recievedAckLatency = currentTimestamp - timeOfFragmentEndSent - data->sleepTimeOffset; // [milliseconds]
                recievedAckLatency_datum.SetMetricName("RecievedAckLatency");
                recievedAckLatency_datum.AddDimensions(DIMENSION_PER_STREAM);
                recievedAckLatency_datum.SetValue(recievedAckLatency);
                recievedAckLatency_datum.SetUnit(Aws::CloudWatch::Model::StandardUnit::Milliseconds);
                cwRequest.AddMetricData(recievedAckLatency_datum);

                auto outcome = data->pCWclient->PutMetricData(cwRequest);
                if (!outcome.IsSuccess())
                {
                    std::cout << "Failed to put RecievedAckLatency metric data:" <<
                        outcome.GetError().GetMessage() << std::endl;
                }
                else
                {
                    std::cout << "Successfully put RecievedAckLatency metric data" << std::endl;
                }
            }
    } else
    {
        cout << "Not sending Ack Latency metric because: timeOfFragmentEndSent < pFragmentAck->timestamp" << endl;
    }
    
}

}  // namespace video
}  // namespace kinesis
}  // namespace amazonaws
}  // namespace com;



void setEnvVarsString(string& configVar, string envVar)
{
    if (getenv(envVar.c_str()) != NULL)
    {
        configVar = getenv(envVar.c_str());
    }
}
void setEnvVarsInt(int& configVar, string envVar)
{
    if (getenv(envVar.c_str()) != NULL)
    {
        configVar = stoi(getenv(envVar.c_str()));
    }
}
void setEnvVarsBool(bool& configVar, string envVar)
{
    if (getenv(envVar.c_str()) != NULL)
    {
        if (getenv(envVar.c_str()) == "TRUE" || getenv(envVar.c_str()) == "true" || getenv(envVar.c_str()) == "True")
        {
            configVar = true;
        } else
        {
            configVar = false;
        }
    }
}
void initConfigWithEnvVars(CanaryConfig* pCanaryConfig)
{
    setEnvVarsString(pCanaryConfig->streamName, "CANARY_STREAM_NAME_ENV_VAR");
    setEnvVarsString(pCanaryConfig->sourceType, "CANARY_SOURCE_TYPE_ENV_VAR");
    setEnvVarsString(pCanaryConfig->canaryRunType, "CANARY_RUN_TYPE_ENV_VAR");
    setEnvVarsString(pCanaryConfig->streamType, "CANARY_STREAM_TYPE_ENV_VAR");
    setEnvVarsString(pCanaryConfig->canaryLabel, "CANARY_LABEL_ENV_VAR");
    setEnvVarsString(pCanaryConfig->cpUrl, "CANARY_CP_URL_ENV_VAR");

    setEnvVarsInt(pCanaryConfig->fragmentSize, "CANARY_FRAGMENT_SIZE_ENV_VAR");
    setEnvVarsInt(pCanaryConfig->canaryDuration, "CANARY_DURATION_ENV_VAR");
    setEnvVarsInt(pCanaryConfig->bufferDuration, "CANARY_BUFFER_DURATION_ENV_VAR");
    setEnvVarsInt(pCanaryConfig->storageSizeInBytes, "CANARY_STORAGE_SIZE_ENV_VAR");
}

// void sleep(unsigned int seconds) 
// 	{ 
// 		usleep(miliseconds * 1000 * 1000); // usleep receives microseconds 
// 	} 


void create_kinesis_video_frame(Frame *frame, const nanoseconds &pts, const nanoseconds &dts, FRAME_FLAGS flags,
                                void *data, size_t len) {
    frame->flags = flags;
    frame->decodingTs = static_cast<UINT64>(dts.count()) / DEFAULT_TIME_UNIT_IN_NANOS;
    frame->presentationTs = static_cast<UINT64>(pts.count()) / DEFAULT_TIME_UNIT_IN_NANOS;
    // set duration to 0 due to potential high spew from rtsp streams
    frame->duration = 0;
    frame->size = static_cast<UINT32>(len);
    frame->frameData = reinterpret_cast<PBYTE>(data);
    frame->trackId = DEFAULT_TRACK_ID;
}

void updateFragmentEndTimes(uint64_t curKeyFrameTime, uint64_t &lastKeyFrameTime, map<uint64_t, uint64_t> *mapPtr)
{
        if (lastKeyFrameTime != 0)
        {
            cout << "lastKeyFrameTime (frame PTs): " << lastKeyFrameTime << endl;
            cout << "curKeyFrameTime: " << curKeyFrameTime << endl;
            (*mapPtr)[lastKeyFrameTime / HUNDREDS_OF_NANOS_IN_A_MILLISECOND] = curKeyFrameTime;
            auto iter = mapPtr->begin();
            while (iter != mapPtr->end()) {
                // clean up map: remove timestamps older than 5 min from now
                if (iter->first < (duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count() - (300 * HUNDREDS_OF_NANOS_IN_A_MILLISECOND)) / HUNDREDS_OF_NANOS_IN_A_MILLISECOND) {
                    iter = mapPtr->erase(iter);
                } else {
                    break;
                }
            }
        }
        lastKeyFrameTime = curKeyFrameTime;
}

void pushKeyFrameMetrics(Frame frame, CustomData *cusData)
            {
            updateFragmentEndTimes(frame.presentationTs, cusData->lastKeyFrameTime, cusData->timeOfNextKeyFrame);
            
            Aws::CloudWatch::Model::MetricDatum frameRate_datum, transferRate_datum, currentViewDuration_datum, availableStoreSize_datum,
                                                    putFrameErrorRate_datum, errorAckRate_datum, totalNumberOfErrors_datum;
            Aws::CloudWatch::Model::PutMetricDataRequest cwRequest;
            cwRequest.SetNamespace("KinesisVideoSDKCanaryCPP");    

            auto stream_metrics = cusData->kinesis_video_stream->getMetrics();
            auto client_metrics = cusData->kinesis_video_stream->getProducer().getMetrics();
            auto stream_metrics_raw = stream_metrics.getRawMetrics(); // perhaps switch to using this to retrieve all the metrics in this code block
            
            auto frameRate = stream_metrics.getCurrentElementaryFrameRate();
            frameRate_datum.SetMetricName("FrameRate");
            frameRate_datum.AddDimensions(DIMENSION_PER_STREAM);
            frameRate_datum.SetValue(frameRate);
            frameRate_datum.SetUnit(Aws::CloudWatch::Model::StandardUnit::Count_Second);
            cwRequest.AddMetricData(frameRate_datum);

            auto transferRate = 8 * stream_metrics.getCurrentTransferRate() / 1024; // *8 makes it bytes->bits. /1024 bits->kilobits
            transferRate_datum.SetMetricName("TransferRate");
            transferRate_datum.AddDimensions(DIMENSION_PER_STREAM);  
            transferRate_datum.SetValue(transferRate);
            transferRate_datum.SetUnit(Aws::CloudWatch::Model::StandardUnit::Kilobits_Second);
            cwRequest.AddMetricData(transferRate_datum);

            auto currentViewDuration = stream_metrics.getCurrentViewDuration().count();
            currentViewDuration_datum.SetMetricName("CurrentViewDuration");
            currentViewDuration_datum.AddDimensions(DIMENSION_PER_STREAM);
            currentViewDuration_datum.SetValue(currentViewDuration);
            currentViewDuration_datum.SetUnit(Aws::CloudWatch::Model::StandardUnit::Milliseconds);
            cwRequest.AddMetricData(currentViewDuration_datum);

            // Metrics seem to always be the same -> look into
            auto availableStoreSize = client_metrics.getContentStoreSizeSize();
            availableStoreSize_datum.SetMetricName("ContentStoreAvailableSize");
            availableStoreSize_datum.AddDimensions(DIMENSION_PER_STREAM);
            availableStoreSize_datum.SetValue(availableStoreSize);
            availableStoreSize_datum.SetUnit(Aws::CloudWatch::Model::StandardUnit::Bytes);
            cwRequest.AddMetricData(availableStoreSize_datum);

            // Capture error rate metrics every 60 seconds
            double duration = duration_cast<seconds>(system_clock::now().time_since_epoch()).count() - cusData->timeCounter;
            if(duration > 60)
            {
                cout << "putFrameErrors: " << stream_metrics_raw->putFrameErrors << endl;
                cout << "errorAcks: " << stream_metrics_raw->errorAcks << endl;

                cusData->timeCounter = duration_cast<seconds>(system_clock::now().time_since_epoch()).count();

                double newPutFrameErrors = (double)stream_metrics_raw->putFrameErrors - cusData->totalPutFrameErrorCount;
                cusData->totalPutFrameErrorCount = stream_metrics_raw->putFrameErrors;
                auto putFrameErrorRate = newPutFrameErrors / (double)duration;
                putFrameErrorRate_datum.SetMetricName("PutFrameErrorRate");
                putFrameErrorRate_datum.AddDimensions(DIMENSION_PER_STREAM);
                putFrameErrorRate_datum.SetValue(putFrameErrorRate);
                putFrameErrorRate_datum.SetUnit(Aws::CloudWatch::Model::StandardUnit::Count_Second);
                cwRequest.AddMetricData(putFrameErrorRate_datum);

                double newErrorAcks = (double)stream_metrics_raw->errorAcks - cusData->totalErrorAckCount;
                cusData->totalErrorAckCount = stream_metrics_raw->errorAcks;
                auto errorAckRate = newErrorAcks / (double)duration;
                errorAckRate_datum.SetMetricName("ErrorAckRate");
                errorAckRate_datum.AddDimensions(DIMENSION_PER_STREAM);
                errorAckRate_datum.SetValue(errorAckRate);
                errorAckRate_datum.SetUnit(Aws::CloudWatch::Model::StandardUnit::Count_Second);
                cwRequest.AddMetricData(errorAckRate_datum);

                auto totalNumberOfErrors = cusData->totalPutFrameErrorCount + cusData->totalErrorAckCount;
                totalNumberOfErrors_datum.SetMetricName("TotalNumberOfErrors");
                totalNumberOfErrors_datum.AddDimensions(DIMENSION_PER_STREAM);
                totalNumberOfErrors_datum.SetValue(totalNumberOfErrors);
                totalNumberOfErrors_datum.SetUnit(Aws::CloudWatch::Model::StandardUnit::Count);
                cwRequest.AddMetricData(totalNumberOfErrors_datum);            
            }

            // Send metrics to CW
            auto outcome = cusData->pCWclient->PutMetricData(cwRequest);
            if (!outcome.IsSuccess())
            {
                std::cout << "Failed to put sample metric data:" <<
                    outcome.GetError().GetMessage() << std::endl;
            }
            else
            {
                std::cout << "Successfully put sample metric data" << std::endl;
            }

        }

 void pushStartupLatencyMetric(Aws::CloudWatch::CloudWatchClient *pCWclient, double producer_start_time)
    {
        double currentTimestamp = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
        double startUpLatency = (double)(currentTimestamp - producer_start_time / 1000000);
        Aws::CloudWatch::Model::MetricDatum startupLatency_datum;
        Aws::CloudWatch::Model::PutMetricDataRequest cwRequest;
        cwRequest.SetNamespace("KinesisVideoSDKCanaryCPP");
        startupLatency_datum.SetMetricName("StartupLatency");
        startupLatency_datum.AddDimensions(DIMENSION_PER_STREAM);
        startupLatency_datum.SetValue(startUpLatency);
        startupLatency_datum.SetUnit(Aws::CloudWatch::Model::StandardUnit::Milliseconds);
        cwRequest.AddMetricData(startupLatency_datum);
        auto outcome = pCWclient->PutMetricData(cwRequest);
        if (!outcome.IsSuccess())
        {
            std::cout << "Failed to put StartupLatency metric data:" <<
                outcome.GetError().GetMessage() << std::endl;
        }
        else
        {
            std::cout << "Successfully put StartupLatency metric data" << std::endl;
        }
    }

bool put_frame(CustomData *cusData, void *data, size_t len, const nanoseconds &pts, const nanoseconds &dts, FRAME_FLAGS flags) {

    Frame frame;
    create_kinesis_video_frame(&frame, pts, dts, flags, data, len);
    bool ret = cusData->kinesis_video_stream->putFrame(frame);

    // canaryStreamMetrics.version = STREAM_METRICS_CURRENT_VERSION;

    // push stream metrics on key frame
    if (CHECK_FRAME_FLAG_KEY_FRAME(flags))
    {
        pushKeyFrameMetrics(frame, cusData);
    }

    return ret;
}

static GstFlowReturn on_new_sample(GstElement *sink, CustomData *data) {    

    GstBuffer *buffer;
    bool isDroppable, isHeader, delta;
    size_t buffer_size;
    GstFlowReturn ret = GST_FLOW_OK;
    STATUS curr_stream_status = data->stream_status.load();
    GstSample *sample = nullptr;
    GstMapInfo info;
    

    if (STATUS_FAILED(curr_stream_status)) {
        LOG_ERROR("Received stream error: " << curr_stream_status);
        ret = GST_FLOW_ERROR;
        goto CleanUp;
    } 

    info.data = nullptr;
    sample = gst_app_sink_pull_sample(GST_APP_SINK (sink));

    // capture cpd at the first frame
    if (!data->stream_started) {
        data->stream_started = true;
        GstCaps* gstcaps  = (GstCaps*) gst_sample_get_caps(sample);
        GstStructure * gststructforcaps = gst_caps_get_structure(gstcaps, 0);
        const GValue *gstStreamFormat = gst_structure_get_value(gststructforcaps, "codec_data");
        gchar *cpd = gst_value_serialize(gstStreamFormat);
        data->kinesis_video_stream->start(std::string(cpd));
        g_free(cpd);
    }

    buffer = gst_sample_get_buffer(sample);
    isHeader = GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_HEADER);
    isDroppable = GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_CORRUPTED) ||
                  GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DECODE_ONLY) ||
                  (GST_BUFFER_FLAGS(buffer) == GST_BUFFER_FLAG_DISCONT) ||
                  (GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DISCONT) && GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT)) ||
                  // drop if buffer contains header only and has invalid timestamp
                  (isHeader && (!GST_BUFFER_PTS_IS_VALID(buffer) || !GST_BUFFER_DTS_IS_VALID(buffer)));
            
    int currTime;
    if (!isDroppable) {

        delta = GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);

        FRAME_FLAGS kinesis_video_flags = delta ? FRAME_FLAG_NONE : FRAME_FLAG_KEY_FRAME;

        // Always synthesize dts for file sources because file sources dont have meaningful dts.
        // For some rtsp sources the dts is invalid, therefore synthesize.
        if (data->streamSource == FILE_SOURCE || !GST_BUFFER_DTS_IS_VALID(buffer)) {
            data->synthetic_dts += DEFAULT_FRAME_DURATION_MS * HUNDREDS_OF_NANOS_IN_A_MILLISECOND * DEFAULT_TIME_UNIT_IN_NANOS;
            buffer->dts = data->synthetic_dts;
        } else if (GST_BUFFER_DTS_IS_VALID(buffer)) {
            data->synthetic_dts = buffer->dts;
        }

        if (data->calcSleepTimeOffset)
        {
            data->sleepTimeOffset += (duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count() - data->sleepTimeStamp); // [milliseconds]
            data->calcSleepTimeOffset = false;
        }

        if (data->streamSource == FILE_SOURCE) {
            data->max_frame_pts = MAX(data->max_frame_pts, buffer->pts);

            // make sure the timestamp is continuous across multiple files.
            buffer->pts += data->base_pts + data->producer_start_time;

            if (CHECK_FRAME_FLAG_KEY_FRAME(kinesis_video_flags)) {
                data->key_frame_pts = buffer->pts;
            }
        } else if (data->use_absolute_fragment_times) {
            if (data->first_pts == GST_CLOCK_TIME_NONE) {
                data->producer_start_time = chrono::duration_cast<nanoseconds>(systemCurrentTime().time_since_epoch()).count();
                data->first_pts = buffer->pts;
            }
            buffer->pts += (data->producer_start_time - data->first_pts);
            cout << "buffer->pts: " << buffer->pts << endl;
            // buffer->pts += data->sleepTimeOffset * 10000000;
            // cout << "buffer->pts: " << buffer->pts << endl;
        }

        if (!gst_buffer_map(buffer, &info, GST_MAP_READ)){
            goto CleanUp;
        }
        if (CHECK_FRAME_FLAG_KEY_FRAME(kinesis_video_flags)) {
            data->kinesis_video_stream->putEventMetadata(STREAM_EVENT_TYPE_NOTIFICATION | STREAM_EVENT_TYPE_IMAGE_GENERATION, NULL);
        }

        cout << "Buffer pts in nanoseconds: " << std::chrono::nanoseconds(buffer->pts).count() << endl;
        cout << "Current time in nanoseconds: " <<  duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count() << endl;

        bool putFrameSuccess = put_frame(data, info.data, info.size, std::chrono::nanoseconds(buffer->pts),
                               std::chrono::nanoseconds(buffer->dts), kinesis_video_flags);

        // If on first frame of stream, push startup latency metric to CW
        if(data->onFirstFrame && putFrameSuccess)
        {
            pushStartupLatencyMetric(data->pCWclient, data->producer_start_time);
            data->onFirstFrame = false;
        }
    }
    
    currTime = duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
    if (currTime > (data->producer_start_time / 1000000000 + data->pCanaryConfig->canaryDuration))
    {
        g_main_loop_quit(data->main_loop);
        //goto CleanUp;
    }

    // data->pCanaryConfig->canaryRunType ="INTERMITENT"; // make it intermitent for testing --------------------------
    if(data->pCanaryConfig->canaryRunType == "INTERMITENT" && duration_cast<minutes>(system_clock::now().time_since_epoch()).count() > data->runTill)
    {
        data->timeOfNextKeyFrame->clear();
        data->calcSleepTimeOffset = true;
        int sleepTime = ((rand() % 10) + 1); // [minutes]
        sleepTime = 1; // ------------------------------------
        cout << "Intermittent sleep time is set to: " << sleepTime << " minutes" << endl;
        data->sleepTimeStamp = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count(); // [milliseconds]
        //sleep(sleepTime * 60); // [seconds]
        sleep(20);
        data->onFirstFrame = true;
        int runTime = (rand() % 10) + 1; // [minutes]
        runTime = 1; // ------------------------------------
        cout << "Intermittent run time is set to: " << runTime << " minutes" << endl;
        // Set runTill to a new random value 1-10 minutes into the future
        data->runTill = duration_cast<minutes>(system_clock::now().time_since_epoch()).count() + runTime; // [minutes]
    }

CleanUp:

    if (info.data != nullptr) {
        gst_buffer_unmap(buffer, &info);
    }

    if (sample != nullptr) {
        gst_sample_unref(sample);
    }

    return ret;
}

/* This function is called when an error message is posted on the bus */
static void error_cb(GstBus *bus, GstMessage *msg, CustomData *data) {
    GError *err;
    gchar *debug_info;

    /* Print error details on the screen */
    gst_message_parse_error(msg, &err, &debug_info);
    g_printerr("Error received from element %s: %s\n", GST_OBJECT_NAME (msg->src), err->message);
    g_printerr("Debugging information: %s\n", debug_info ? debug_info : "none");
    g_clear_error(&err);
    g_free(debug_info);

    g_main_loop_quit(data->main_loop);
}

void kinesis_video_init(CustomData *data) {
    unique_ptr<DeviceInfoProvider> device_info_provider(new SampleDeviceInfoProvider());
    unique_ptr<ClientCallbackProvider> client_callback_provider(new SampleClientCallbackProvider());
    unique_ptr<StreamCallbackProvider> stream_callback_provider(new SampleStreamCallbackProvider(reinterpret_cast<UINT64>(data)));

    char const *accessKey;
    char const *secretKey;
    char const *sessionToken;
    char const *defaultRegion;
    string defaultRegionStr;
    string sessionTokenStr;

    char const *iot_get_credential_endpoint;
    char const *cert_path;
    char const *private_key_path;
    char const *role_alias;
    char const *ca_cert_path;

    unique_ptr<CredentialProvider> credential_provider;

    if (nullptr == (defaultRegion = getenv(DEFAULT_REGION_ENV_VAR))) {
        defaultRegionStr = DEFAULT_AWS_REGION;
    } else {
        defaultRegionStr = string(defaultRegion);
    }
    LOG_INFO("Using region: " << defaultRegionStr);

    if (nullptr != (accessKey = getenv(ACCESS_KEY_ENV_VAR)) &&
        nullptr != (secretKey = getenv(SECRET_KEY_ENV_VAR))) {

        LOG_INFO("Using aws credentials for Kinesis Video Streams");
        if (nullptr != (sessionToken = getenv(SESSION_TOKEN_ENV_VAR))) {
            LOG_INFO("Session token detected.");
            sessionTokenStr = string(sessionToken);
        } else {
            LOG_INFO("No session token was detected.");
            sessionTokenStr = "";
        }

        data->credential.reset(new Credentials(string(accessKey),
                                               string(secretKey),
                                               sessionTokenStr,
                                               std::chrono::seconds(DEFAULT_CREDENTIAL_EXPIRATION_SECONDS)));
        credential_provider.reset(new SampleCredentialProvider(*data->credential.get()));

    } else if (nullptr != (iot_get_credential_endpoint = getenv("IOT_GET_CREDENTIAL_ENDPOINT")) &&
               nullptr != (cert_path = getenv("CERT_PATH")) &&
               nullptr != (private_key_path = getenv("PRIVATE_KEY_PATH")) &&
               nullptr != (role_alias = getenv("ROLE_ALIAS")) &&
               nullptr != (ca_cert_path = getenv("CA_CERT_PATH"))) {
        LOG_INFO("Using IoT credentials for Kinesis Video Streams");
        credential_provider.reset(new IotCertCredentialProvider(iot_get_credential_endpoint,
                                                                cert_path,
                                                                private_key_path,
                                                                role_alias,
                                                                ca_cert_path,
                                                                data->stream_name));

    } else {
        LOG_AND_THROW("No valid credential method was found");
    }

    data->kinesis_video_producer = KinesisVideoProducer::createSync(move(device_info_provider),
                                                                    move(client_callback_provider),
                                                                    move(stream_callback_provider),
                                                                    move(credential_provider),
                                                                    defaultRegionStr);

    LOG_DEBUG("Client is ready");
}

void kinesis_video_stream_init(CustomData *data) {
    /* create a test stream */
    map<string, string> tags;
    char tag_name[MAX_TAG_NAME_LEN];
    char tag_val[MAX_TAG_VALUE_LEN];
    SPRINTF(tag_name, "piTag");
    SPRINTF(tag_val, "piValue");

    STREAMING_TYPE streaming_type = DEFAULT_STREAMING_TYPE;
    data->use_absolute_fragment_times = DEFAULT_ABSOLUTE_FRAGMENT_TIMES;

    unique_ptr<StreamDefinition> stream_definition(new StreamDefinition(
        data->stream_name,
        hours(DEFAULT_RETENTION_PERIOD_HOURS),
        &tags,
        DEFAULT_KMS_KEY_ID,
        streaming_type,
        DEFAULT_CONTENT_TYPE,
        duration_cast<milliseconds> (seconds(DEFAULT_MAX_LATENCY_SECONDS)),
        milliseconds(DEFAULT_FRAGMENT_DURATION_MILLISECONDS),
        milliseconds(DEFAULT_TIMECODE_SCALE_MILLISECONDS),
        DEFAULT_KEY_FRAME_FRAGMENTATION,
        DEFAULT_FRAME_TIMECODES,
        data->use_absolute_fragment_times,
        DEFAULT_FRAGMENT_ACKS,
        DEFAULT_RESTART_ON_ERROR,
        DEFAULT_RECALCULATE_METRICS,
        0,
        DEFAULT_STREAM_FRAMERATE,
        DEFAULT_AVG_BANDWIDTH_BPS,
        seconds(DEFAULT_BUFFER_DURATION_SECONDS),
        seconds(DEFAULT_REPLAY_DURATION_SECONDS),
        seconds(DEFAULT_CONNECTION_STALENESS_SECONDS),
        DEFAULT_CODEC_ID,
        DEFAULT_TRACKNAME,
        nullptr,
        0));
    data->kinesis_video_stream = data->kinesis_video_producer->createStreamSync(move(stream_definition));

    // reset state
    data->stream_status = STATUS_SUCCESS;
    data->stream_started = false;


    LOG_DEBUG("Stream is ready");
}

/* callback when each RTSP stream has been created */
static void pad_added_cb(GstElement *element, GstPad *pad, GstElement *target) {
    GstPad *target_sink = gst_element_get_static_pad(GST_ELEMENT(target), "sink");
    GstPadLinkReturn link_ret;
    gchar *pad_name = gst_pad_get_name(pad);
    g_print("New pad found: %s\n", pad_name);

    link_ret = gst_pad_link(pad, target_sink);

    if (link_ret == GST_PAD_LINK_OK) {
        LOG_INFO("Pad link successful");
    } else {
        LOG_INFO("Pad link failed");
    }

    gst_object_unref(target_sink);
    g_free(pad_name);
}

int gstreamer_test_source_init(CustomData *data, GstElement *pipeline) {
    
    GstElement *appsink, *source, *video_src_filter, *h264parse, *video_filter, *h264enc, *autovidcon;

    GstCaps *caps;

    // define the elements
    source = gst_element_factory_make("videotestsrc", "source");
    autovidcon = gst_element_factory_make("autovideoconvert", "vidconv");
    h264enc = gst_element_factory_make("x264enc", "h264enc");
    h264parse = gst_element_factory_make("h264parse", "h264parse");
    appsink = gst_element_factory_make("appsink", "appsink");

    // to change output video pattern to a moving ball, uncomment below
    //g_object_set(source, "pattern", 18, NULL);

    // NEED TO SET THIS TO LIVE to increment buffer pts and dts; when not set to live,
    // it will mess up fragment ack metrics
    g_object_set(source, "is-live", TRUE, NULL);

    // configure appsink
    g_object_set(G_OBJECT (appsink), "emit-signals", TRUE, "sync", FALSE, NULL);
    g_signal_connect(appsink, "new-sample", G_CALLBACK(on_new_sample), data);
    
    // define the elements
    h264enc = gst_element_factory_make("vtenc_h264_hw", "h264enc");
    h264parse = gst_element_factory_make("h264parse", "h264parse");

    // define and configure video filter, we only want the specified format to pass to the sink
    // ("caps" is short for "capabilities")
    string video_caps_string = "video/x-h264, stream-format=(string) avc, alignment=(string) au";
    video_filter = gst_element_factory_make("capsfilter", "video_filter");
    caps = gst_caps_from_string(video_caps_string.c_str());
    g_object_set(G_OBJECT (video_filter), "caps", caps, NULL);
    gst_caps_unref(caps);

    // TODO: make the width and height configurable
    video_caps_string = "video/x-raw, framerate=" + to_string(TESTING_FPS) + "/1" + ", width=1440, height=1080";
    video_src_filter = gst_element_factory_make("capsfilter", "video_source_filter");
    caps = gst_caps_from_string(video_caps_string.c_str());
    g_object_set(G_OBJECT (video_src_filter), "caps", caps, NULL);
    gst_caps_unref(caps);

    // check if all elements were created
    if (!pipeline || !source || !video_src_filter || !appsink || !autovidcon || !h264parse || 
        !video_filter || !h264enc)
    {
        g_printerr("Not all elements could be created.\n");
        return 1;
    }

    // build the pipeline
    gst_bin_add_many(GST_BIN (pipeline), source, video_src_filter, autovidcon, h264enc,
                    h264parse, video_filter, appsink, NULL);

    // check if all elements were linked
    if (!gst_element_link_many(source, video_src_filter, autovidcon, h264enc, 
        h264parse, video_filter, appsink, NULL)) 
    {
        g_printerr("Elements could not be linked.\n");
        gst_object_unref(pipeline);
        return 1;
    }

    return 0;
}

int gstreamer_init(int argc, char* argv[], CustomData *data) {

    /* init GStreamer */
    gst_init(&argc, &argv);

    GstElement *pipeline;
    int ret;
    GstStateChangeReturn gst_ret;

    // Reset first frame pts
    data->first_pts = GST_CLOCK_TIME_NONE;

    switch (data->streamSource) {
        case TEST_SOURCE:
            LOG_INFO("Streaming from test source");
            pipeline = gst_pipeline_new("test-kinesis-pipeline");
            ret = gstreamer_test_source_init(data, pipeline);
            break;
    }
    if (ret != 0){
        return ret;
    }

    /* Instruct the bus to emit signals for each received message, and connect to the interesting signals */
    GstBus *bus = gst_element_get_bus(pipeline);
    gst_bus_add_signal_watch(bus);
    g_signal_connect (G_OBJECT(bus), "message::error", (GCallback) error_cb, data);
    gst_object_unref(bus);

    /* start streaming */
    gst_ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (gst_ret == GST_STATE_CHANGE_FAILURE) {
        g_printerr("Unable to set the pipeline to the playing state.\n");
        gst_object_unref(pipeline);
        return 1;
    }

    data->main_loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(data->main_loop);

    /* free resources */
    gst_bus_remove_signal_watch(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    g_main_loop_unref(data->main_loop);
    data->main_loop = NULL;
    return 0;
}

int main(int argc, char* argv[]) {
    PropertyConfigurator::doConfigure("../kvs_log_configuration");
    initializeEndianness();
    srand(time(0));
    Aws::SDKOptions options;
    Aws::InitAPI(options);
    {
        // can put CustomData initialization lower to avoid keeping certain things within producer_start_time
        CanaryConfig canaryConfig;
        CustomData data(canaryConfig);
        initConfigWithEnvVars(data.pCanaryConfig);


        const int PUTFRAME_FAILURE_RETRY_COUNT = 3;

        int file_retry_count = PUTFRAME_FAILURE_RETRY_COUNT;
        STATUS stream_status = STATUS_SUCCESS;

        Aws::CloudWatch::CloudWatchClient CWclient(data.client_config);
        data.pCWclient = &CWclient;

        data.stream_name = const_cast<char*>(data.pCanaryConfig->streamName.c_str());

        // set the video stream source
        if (data.pCanaryConfig->sourceType == "TEST_SOURCE")
        {
            data.streamSource = TEST_SOURCE;     
        }


        DIMENSION_PER_STREAM.SetName("ProducerSDKCanaryStreamNameCPP");
        DIMENSION_PER_STREAM.SetValue(data.stream_name);
        

        /* init Kinesis Video */
        try{
            kinesis_video_init(&data);
            kinesis_video_stream_init(&data);
        } catch (runtime_error &err) {
            LOG_ERROR("Failed to initialize kinesis video with an exception: " << err.what());
            return 1;
        }

        bool do_retry = true;

        if (data.streamSource == TEST_SOURCE)
        {
            gstreamer_init(argc, argv, &data);
            if (STATUS_SUCCEEDED(stream_status)) {
                    // if stream_status is success after eos, send out remaining frames.
                    data.kinesis_video_stream->stopSync();
                } else {
                    data.kinesis_video_stream->stop();
                }
        }

    // CleanUp
    data.kinesis_video_producer->freeStream(data.kinesis_video_stream);
    delete (data.timeOfNextKeyFrame);
    }

    return 0;
}