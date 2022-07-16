#include "CanaryConfig.h"

CanaryConfig::CanaryConfig()
{
    testVideoFps = 25;
    streamName = "DefaultStreamName";
    sourceType = "TEST_SOURCE";
    canaryRunType = "NORMAL"; // (or intermittent)
    streamType = "REALTIME";
    canaryLabel = "DEFAULT_CANARY_LABEL"; // need to decide on a default value
    cpUrl = "";
    fragmentSize = DEFAULT_FRAGMENT_DURATION_MILLISECONDS;
    canaryDuration = DEFAULT_CANARY_DURATION_SECONDS;
    bufferDuration = DEFAULT_BUFFER_DURATION_SECONDS;
    storageSizeInBytes = 0;
}

void CanaryConfig::setEnvVarsString(string &configVar, string envVar)
{
    if (getenv(envVar.c_str()) != NULL)
    {
        configVar = getenv(envVar.c_str());
    }
}

void CanaryConfig::setEnvVarsInt(int &configVar, string envVar)
{
    if (getenv(envVar.c_str()) != NULL)
    {
        configVar = stoi(getenv(envVar.c_str()));
    }
}

void CanaryConfig::setEnvVarsBool(bool &configVar, string envVar)
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


void CanaryConfig::initConfigWithEnvVars()
{
    setEnvVarsString(streamName, "CANARY_STREAM_NAME");
    //setEnvVarsString(sourceType, "CANARY_SOURCE_TYPE");
    setEnvVarsString(canaryRunType, "CANARY_RUN_TYPE");
    setEnvVarsString(streamType, "CANARY_STREAM_TYPE");
    setEnvVarsString(canaryLabel, "CANARY_LABEL");
    setEnvVarsString(cpUrl, "CANARY_CP_URL");

    setEnvVarsInt(fragmentSize, "CANARY_FRAGMENT_SIZE");
    setEnvVarsInt(canaryDuration, "CANARY_DURATION");
    setEnvVarsInt(bufferDuration, "CANARY_BUFFER_DURATION");
    setEnvVarsInt(storageSizeInBytes, "CANARY_STORAGE_SIZE");
    setEnvVarsInt(testVideoFps, "CANARY_FPS");
}