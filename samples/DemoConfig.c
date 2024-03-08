#define LOG_CLASS "WebRtcSamples"
#include "Samples.h"

PSampleConfiguration gSampleConfiguration = NULL;

VOID sigintHandler(INT32 sigNum)
{
    UNUSED_PARAM(sigNum);
    if (gSampleConfiguration != NULL) {
        ATOMIC_STORE_BOOL(&gSampleConfiguration->interrupted, TRUE);
        CVAR_BROADCAST(gSampleConfiguration->cvar);
    }
}

STATUS signalingClientError(UINT64 customData, STATUS status, PCHAR msg, UINT32 msgLen)
{
    PSampleConfiguration pSampleConfiguration = (PSampleConfiguration) customData;

    DLOGW("Signaling client generated an error 0x%08x - '%.*s'", status, msgLen, msg);

    // We will force re-create the signaling client on the following errors
    if (status == STATUS_SIGNALING_ICE_CONFIG_REFRESH_FAILED || status == STATUS_SIGNALING_RECONNECT_FAILED) {
        ATOMIC_STORE_BOOL(&pSampleConfiguration->appSignalingCtx.recreateSignalingClient, TRUE);
        CVAR_BROADCAST(pSampleConfiguration->cvar);
    }

    return STATUS_SUCCESS;
}

PVOID sendVideoPackets(PVOID args)
{
    STATUS retStatus = STATUS_SUCCESS;
    PSampleConfiguration pSampleConfiguration = (PSampleConfiguration) args;
    RtcEncoderStats encoderStats;
    Frame frame;
    UINT32 fileIndex = 0, frameSize;
    CHAR filePath[MAX_PATH_LEN + 1];
    STATUS status;
    UINT32 i;
    UINT64 startTime, lastFrameTime, elapsed;
    MEMSET(&encoderStats, 0x00, SIZEOF(RtcEncoderStats));
    CHK_ERR(pSampleConfiguration != NULL, STATUS_NULL_ARG, "[KVS Master] Streaming session is NULL");

    frame.presentationTs = 0;
    startTime = GETTIME();
    lastFrameTime = startTime;

    while (!ATOMIC_LOAD_BOOL(&pSampleConfiguration->appTerminateFlag)) {
        fileIndex = fileIndex % NUMBER_OF_H264_FRAME_FILES + 1;
        SNPRINTF(filePath, MAX_PATH_LEN, "./h264SampleFrames/frame-%04d.h264", fileIndex);

        CHK_STATUS(readFrameFromDisk(NULL, &frameSize, filePath));

        // Re-alloc if needed
        if (frameSize > pSampleConfiguration->appMediaCtx.videoBufferSize) {
            pSampleConfiguration->appMediaCtx.pVideoFrameBuffer = (PBYTE) MEMREALLOC(pSampleConfiguration->appMediaCtx.pVideoFrameBuffer, frameSize);
            CHK_ERR(pSampleConfiguration->appMediaCtx.pVideoFrameBuffer != NULL, STATUS_NOT_ENOUGH_MEMORY,
                    "[KVS Master] Failed to allocate video frame buffer");
            pSampleConfiguration->appMediaCtx.videoBufferSize = frameSize;
        }

        frame.frameData = pSampleConfiguration->appMediaCtx.pVideoFrameBuffer;
        frame.size = frameSize;

        CHK_STATUS(readFrameFromDisk(frame.frameData, &frameSize, filePath));

        // based on bitrate of samples/h264SampleFrames/frame-*
        encoderStats.width = 640;
        encoderStats.height = 480;
        encoderStats.targetBitrate = 262000;
        frame.presentationTs += SAMPLE_VIDEO_FRAME_DURATION;
        MUTEX_LOCK(pSampleConfiguration->streamingSessionListReadLock);
        for (i = 0; i < pSampleConfiguration->streamingSessionCount; ++i) {
            status = writeFrame(pSampleConfiguration->sampleStreamingSessionList[i]->pVideoRtcRtpTransceiver, &frame);
            if (pSampleConfiguration->sampleStreamingSessionList[i]->firstFrame && status == STATUS_SUCCESS) {
                PROFILE_WITH_START_TIME(pSampleConfiguration->sampleStreamingSessionList[i]->offerReceiveTime, "Time to first frame");
                pSampleConfiguration->sampleStreamingSessionList[i]->firstFrame = FALSE;
            }
            encoderStats.encodeTimeMsec = 4; // update encode time to an arbitrary number to demonstrate stats update
            updateEncoderStats(pSampleConfiguration->sampleStreamingSessionList[i]->pVideoRtcRtpTransceiver, &encoderStats);
            if (status != STATUS_SRTP_NOT_READY_YET) {
                if (status != STATUS_SUCCESS) {
                    DLOGV("writeFrame() failed with 0x%08x", status);
                }
            } else {
                // Reset file index to ensure first frame sent upon SRTP ready is a key frame.
                fileIndex = 0;
            }
        }
        MUTEX_UNLOCK(pSampleConfiguration->streamingSessionListReadLock);

        // Adjust sleep in the case the sleep itself and writeFrame take longer than expected. Since sleep makes sure that the thread
        // will be paused at least until the given amount, we can assume that there's no too early frame scenario.
        // Also, it's very unlikely to have a delay greater than SAMPLE_VIDEO_FRAME_DURATION, so the logic assumes that this is always
        // true for simplicity.
        elapsed = lastFrameTime - startTime;
        THREAD_SLEEP(SAMPLE_VIDEO_FRAME_DURATION - elapsed % SAMPLE_VIDEO_FRAME_DURATION);
        lastFrameTime = GETTIME();
    }

CleanUp:
    DLOGI("[KVS Master] Closing video thread");
    CHK_LOG_ERR(retStatus);

    return (PVOID) (ULONG_PTR) retStatus;
}

PVOID sendAudioPackets(PVOID args)
{
    STATUS retStatus = STATUS_SUCCESS;
    PSampleConfiguration pSampleConfiguration = (PSampleConfiguration) args;
    Frame frame;
    UINT32 fileIndex = 0, frameSize;
    CHAR filePath[MAX_PATH_LEN + 1];
    UINT32 i;
    STATUS status;

    CHK_ERR(pSampleConfiguration != NULL, STATUS_NULL_ARG, "[KVS Master] Streaming session is NULL");
    frame.presentationTs = 0;

    while (!ATOMIC_LOAD_BOOL(&pSampleConfiguration->appTerminateFlag)) {
        fileIndex = fileIndex % NUMBER_OF_OPUS_FRAME_FILES + 1;
        SNPRINTF(filePath, MAX_PATH_LEN, "./opusSampleFrames/sample-%03d.opus", fileIndex);

        CHK_STATUS(readFrameFromDisk(NULL, &frameSize, filePath));

        // Re-alloc if needed
        if (frameSize > pSampleConfiguration->appMediaCtx.audioBufferSize) {
            pSampleConfiguration->appMediaCtx.pAudioFrameBuffer = (UINT8*) MEMREALLOC(pSampleConfiguration->appMediaCtx.pAudioFrameBuffer, frameSize);
            CHK_ERR(pSampleConfiguration->appMediaCtx.pAudioFrameBuffer != NULL, STATUS_NOT_ENOUGH_MEMORY,
                    "[KVS Master] Failed to allocate audio frame buffer");
            pSampleConfiguration->appMediaCtx.audioBufferSize = frameSize;
        }

        frame.frameData = pSampleConfiguration->appMediaCtx.pAudioFrameBuffer;
        frame.size = frameSize;

        CHK_STATUS(readFrameFromDisk(frame.frameData, &frameSize, filePath));

        frame.presentationTs += SAMPLE_AUDIO_FRAME_DURATION;

        MUTEX_LOCK(pSampleConfiguration->streamingSessionListReadLock);
        for (i = 0; i < pSampleConfiguration->streamingSessionCount; ++i) {
            status = writeFrame(pSampleConfiguration->sampleStreamingSessionList[i]->pAudioRtcRtpTransceiver, &frame);
            if (status != STATUS_SRTP_NOT_READY_YET) {
                if (status != STATUS_SUCCESS) {
                    DLOGV("writeFrame() failed with 0x%08x", status);
                } else if (pSampleConfiguration->sampleStreamingSessionList[i]->firstFrame && status == STATUS_SUCCESS) {
                    PROFILE_WITH_START_TIME(pSampleConfiguration->sampleStreamingSessionList[i]->offerReceiveTime, "Time to first frame");
                    pSampleConfiguration->sampleStreamingSessionList[i]->firstFrame = FALSE;
                }
            } else {
                // Reset file index to stay in sync with video frames.
                fileIndex = 0;
            }
        }
        MUTEX_UNLOCK(pSampleConfiguration->streamingSessionListReadLock);
        THREAD_SLEEP(SAMPLE_AUDIO_FRAME_DURATION);
    }

CleanUp:
    DLOGI("[KVS Master] closing audio thread");
    return (PVOID) (ULONG_PTR) retStatus;
}

PVOID sampleReceiveAudioVideoFrame(PVOID args)
{
    STATUS retStatus = STATUS_SUCCESS;
    PSampleStreamingSession pSampleStreamingSession = (PSampleStreamingSession) args;
    CHK_ERR(pSampleStreamingSession != NULL, STATUS_NULL_ARG, "[KVS Master] Streaming session is NULL");
    CHK_STATUS(transceiverOnFrame(pSampleStreamingSession->pVideoRtcRtpTransceiver, (UINT64) pSampleStreamingSession, sampleVideoFrameHandler));
    CHK_STATUS(transceiverOnFrame(pSampleStreamingSession->pAudioRtcRtpTransceiver, (UINT64) pSampleStreamingSession, sampleAudioFrameHandler));

CleanUp:

    return (PVOID) (ULONG_PTR) retStatus;
}

static STATUS setUpLogging(PSampleConfiguration pSampleConfiguration)
{
    PCHAR pLogLevel;
    UINT32 logLevel = LOG_LEVEL_DEBUG;
    STATUS retStatus = STATUS_SUCCESS;
    if (NULL == (pLogLevel = GETENV(DEBUG_LOG_LEVEL_ENV_VAR)) || STATUS_SUCCESS != STRTOUI32(pLogLevel, NULL, 10, &logLevel) ||
        logLevel < LOG_LEVEL_VERBOSE || logLevel > LOG_LEVEL_SILENT) {
        logLevel = LOG_LEVEL_WARN;
    }
    SET_LOGGER_LOG_LEVEL(logLevel);

    if (NULL != GETENV(ENABLE_FILE_LOGGING)) {
        CHK_LOG_ERR(createFileLoggerWithLevelFiltering(FILE_LOGGING_BUFFER_SIZE, MAX_NUMBER_OF_LOG_FILES, (PCHAR) FILE_LOGGER_LOG_FILE_DIRECTORY_PATH,
                                                       TRUE, TRUE, TRUE, LOG_LEVEL_PROFILE, NULL));
        pSampleConfiguration->enableFileLogging = TRUE;
    } else {
        CHK_LOG_ERR(createFileLoggerWithLevelFiltering(FILE_LOGGING_BUFFER_SIZE, MAX_NUMBER_OF_LOG_FILES, (PCHAR) FILE_LOGGER_LOG_FILE_DIRECTORY_PATH,
                                                       TRUE, TRUE, FALSE, LOG_LEVEL_PROFILE, NULL));
        pSampleConfiguration->enableFileLogging = TRUE;
    }
    pSampleConfiguration->logLevel = logLevel;
CleanUp:
    return retStatus;
}

static STATUS setUpCredentialProvider(PSampleConfiguration pSampleConfiguration)
{
    STATUS retStatus = STATUS_SUCCESS;
    PCHAR pAccessKey, pSecretKey, pSessionToken;
#ifdef IOT_CORE_ENABLE_CREDENTIALS
    PCHAR pIotCoreCredentialEndPoint, pIotCoreCert, pIotCorePrivateKey, pIotCoreRoleAlias, pIotCoreCertificateId, pIotCoreThingName;
    CHK_ERR((pIotCoreCredentialEndPoint = GETENV(IOT_CORE_CREDENTIAL_ENDPOINT)) != NULL, STATUS_INVALID_OPERATION,
            "AWS_IOT_CORE_CREDENTIAL_ENDPOINT must be set");
    CHK_ERR((pIotCoreCert = GETENV(IOT_CORE_CERT)) != NULL, STATUS_INVALID_OPERATION, "AWS_IOT_CORE_CERT must be set");
    CHK_ERR((pIotCorePrivateKey = GETENV(IOT_CORE_PRIVATE_KEY)) != NULL, STATUS_INVALID_OPERATION, "AWS_IOT_CORE_PRIVATE_KEY must be set");
    CHK_ERR((pIotCoreRoleAlias = GETENV(IOT_CORE_ROLE_ALIAS)) != NULL, STATUS_INVALID_OPERATION, "AWS_IOT_CORE_ROLE_ALIAS must be set");
    CHK_ERR((pIotCoreThingName = GETENV(IOT_CORE_THING_NAME)) != NULL, STATUS_INVALID_OPERATION, "AWS_IOT_CORE_THING_NAME must be set");
#else
    CHK_ERR((pAccessKey = GETENV(ACCESS_KEY_ENV_VAR)) != NULL, STATUS_INVALID_OPERATION, "AWS_ACCESS_KEY_ID must be set");
    CHK_ERR((pSecretKey = GETENV(SECRET_KEY_ENV_VAR)) != NULL, STATUS_INVALID_OPERATION, "AWS_SECRET_ACCESS_KEY must be set");
#endif

    pSessionToken = GETENV(SESSION_TOKEN_ENV_VAR);
    if (pSessionToken != NULL && IS_EMPTY_STRING(pSessionToken)) {
        DLOGW("Session token is set but its value is empty. Ignoring.");
        pSessionToken = NULL;
    }

    if ((pSampleConfiguration->appSignalingCtx.channelInfo.pRegion = GETENV(DEFAULT_REGION_ENV_VAR)) == NULL) {
        pSampleConfiguration->appSignalingCtx.channelInfo.pRegion = DEFAULT_AWS_REGION;
    }

    CHK_STATUS(lookForSslCert(&pSampleConfiguration));

#ifdef IOT_CORE_ENABLE_CREDENTIALS
    CHK_STATUS(createLwsIotCredentialProvider(pIotCoreCredentialEndPoint, pIotCoreCert, pIotCorePrivateKey, pSampleConfiguration->pCaCertPath,
                                              pIotCoreRoleAlias, pIotCoreThingName, &pSampleConfiguration->pCredentialProvider));
#else
    CHK_STATUS(
        createStaticCredentialProvider(pAccessKey, 0, pSecretKey, 0, pSessionToken, 0, MAX_UINT64, &pSampleConfiguration->pCredentialProvider));
#endif
CleanUp:
    return retStatus;
}

static STATUS setUpSignalingDefaults(PSampleConfiguration pSampleConfiguration)
{
    STATUS retStatus = STATUS_SUCCESS;
    pSampleConfiguration->appSignalingCtx.signalingClientHandle = INVALID_SIGNALING_CLIENT_HANDLE_VALUE;
    pSampleConfiguration->appSignalingCtx.channelInfo.version = CHANNEL_INFO_CURRENT_VERSION;
    pSampleConfiguration->appSignalingCtx.channelInfo.pKmsKeyId = NULL;
    pSampleConfiguration->appSignalingCtx.channelInfo.tagCount = 0;
    pSampleConfiguration->appSignalingCtx.channelInfo.pTags = NULL;
    pSampleConfiguration->appSignalingCtx.channelInfo.channelType = SIGNALING_CHANNEL_TYPE_SINGLE_MASTER;
    pSampleConfiguration->appSignalingCtx.channelInfo.cachingPolicy = SIGNALING_API_CALL_CACHE_TYPE_FILE;
    pSampleConfiguration->appSignalingCtx.channelInfo.cachingPeriod = SIGNALING_API_CALL_CACHE_TTL_SENTINEL_VALUE;
    pSampleConfiguration->appSignalingCtx.channelInfo.asyncIceServerConfig = TRUE; // has no effect
    pSampleConfiguration->appSignalingCtx.channelInfo.retry = TRUE;
    pSampleConfiguration->appSignalingCtx.channelInfo.reconnect = TRUE;
    pSampleConfiguration->appSignalingCtx.channelInfo.pCertPath = pSampleConfiguration->pCaCertPath;
    pSampleConfiguration->appSignalingCtx.channelInfo.messageTtl = 0; // Default is 60 seconds
    pSampleConfiguration->appSignalingCtx.channelInfo.channelRoleType = pSampleConfiguration->appConfigCtx.roleType;
    pSampleConfiguration->appSignalingCtx.channelInfo.pChannelName = pSampleConfiguration->appConfigCtx.pChannelName;

    pSampleConfiguration->appSignalingCtx.signalingClientCallbacks.version = SIGNALING_CLIENT_CALLBACKS_CURRENT_VERSION;
    pSampleConfiguration->appSignalingCtx.signalingClientCallbacks.errorReportFn = signalingClientError;
    pSampleConfiguration->appSignalingCtx.signalingClientCallbacks.stateChangeFn = signalingClientStateChanged;
    pSampleConfiguration->appSignalingCtx.signalingClientCallbacks.customData = (UINT64) pSampleConfiguration;

    pSampleConfiguration->appSignalingCtx.clientInfo.version = SIGNALING_CLIENT_INFO_CURRENT_VERSION;
    pSampleConfiguration->appSignalingCtx.clientInfo.loggingLevel = pSampleConfiguration->logLevel;
    pSampleConfiguration->appSignalingCtx.clientInfo.cacheFilePath = NULL; // Use the default path
    pSampleConfiguration->appSignalingCtx.clientInfo.signalingClientCreationMaxRetryAttempts = CREATE_SIGNALING_CLIENT_RETRY_ATTEMPTS_SENTINEL_VALUE;
CleanUp:
    return retStatus;
}

static STATUS setUpDefaultsFn(UINT64 sampleConfigHandle, SIGNALING_CHANNEL_ROLE_TYPE roleType)
{
    STATUS retStatus = STATUS_SUCCESS;
    PSampleConfiguration pSampleConfiguration = (PSampleConfiguration) sampleConfigHandle;
    /* This is ignored for master. Master can extract the info from offer. Viewer has to know if peer can trickle or
     * not ahead of time. */
    pSampleConfiguration->appConfigCtx.trickleIce = TRUE;
    pSampleConfiguration->appConfigCtx.useTurn = TRUE;
    pSampleConfiguration->appConfigCtx.enableSendingMetricsToViewerViaDc = FALSE;
    pSampleConfiguration->appConfigCtx.roleType = roleType;
    pSampleConfiguration->appConfigCtx.pChannelName = "test";

#ifdef IOT_CORE_ENABLE_CREDENTIALS
    if ((pIotCoreCertificateId = GETENV(IOT_CORE_CERTIFICATE_ID)) != NULL) {
        pSampleConfiguration->channelInfo.pChannelName = pIotCoreCertificateId;
    }
#endif

#ifdef ENABLE_DATA_CHANNEL
    pSampleConfiguration->onDataChannel = onDataChannel;
#endif

CleanUp:
    return retStatus;
}

static STATUS setUpDemoDefaults(PSampleConfiguration pSampleConfiguration)
{
    STATUS retStatus = STATUS_SUCCESS;
    pSampleConfiguration->appMediaCtx.mediaSenderTid = INVALID_TID_VALUE;
    pSampleConfiguration->appMediaCtx.audioSenderTid = INVALID_TID_VALUE;
    pSampleConfiguration->appMediaCtx.videoSenderTid = INVALID_TID_VALUE;
    pSampleConfiguration->sampleConfigurationObjLock = MUTEX_CREATE(TRUE);
    pSampleConfiguration->cvar = CVAR_CREATE();
    pSampleConfiguration->streamingSessionListReadLock = MUTEX_CREATE(FALSE);
    pSampleConfiguration->appSignalingCtx.signalingSendMessageLock = MUTEX_CREATE(FALSE);
    ATOMIC_STORE_BOOL(&pSampleConfiguration->interrupted, FALSE);
    ATOMIC_STORE_BOOL(&pSampleConfiguration->mediaThreadStarted, FALSE);
    ATOMIC_STORE_BOOL(&pSampleConfiguration->appTerminateFlag, FALSE);
    ATOMIC_STORE_BOOL(&pSampleConfiguration->appSignalingCtx.recreateSignalingClient, FALSE);
    ATOMIC_STORE_BOOL(&pSampleConfiguration->connected, FALSE);
    CHK_STATUS(stackQueueCreate(&pSampleConfiguration->pPendingSignalingMessageForRemoteClient));
    CHK_STATUS(doubleListCreate(&pSampleConfiguration->timerIdList));
    CHK_STATUS(timerQueueCreate(&pSampleConfiguration->timerQueueHandle));
    CHK_STATUS(stackQueueCreate(&pSampleConfiguration->pregeneratedCertificates));
    CHK_STATUS(hashTableCreateWithParams(SAMPLE_HASH_TABLE_BUCKET_COUNT, SAMPLE_HASH_TABLE_BUCKET_LENGTH,
                                         &pSampleConfiguration->pRtcPeerConnectionForRemoteClient));
    pSampleConfiguration->iceUriCount = 0;
CleanUp:
    return retStatus;
}

STATUS initializeConfiguration(PSampleConfiguration* ppSampleConfiguration, SIGNALING_CHANNEL_ROLE_TYPE roleType, ParamsSetFn paramsSetFn)
{
    STATUS retStatus = STATUS_SUCCESS;
    PSampleConfiguration pSampleConfiguration = NULL;

    CHK(ppSampleConfiguration != NULL, STATUS_NULL_ARG);

#ifndef _WIN32
    signal(SIGINT, sigintHandler);
#endif

    SET_INSTRUMENTED_ALLOCATORS();
    CHK(NULL != (pSampleConfiguration = (PSampleConfiguration) MEMCALLOC(1, SIZEOF(SampleConfiguration))), STATUS_NOT_ENOUGH_MEMORY);
    CHK_STATUS(initKvsWebRtc());
    CHK_STATUS(setUpLogging(pSampleConfiguration));
    CHK_STATUS(setUpCredentialProvider(pSampleConfiguration));
    CHK_STATUS(setUpDemoDefaults(pSampleConfiguration));
    if (paramsSetFn == NULL) {
        pSampleConfiguration->configureAppFn = setUpDefaultsFn;
    } else {
        pSampleConfiguration->configureAppFn = paramsSetFn;
    }
    CHK_STATUS(pSampleConfiguration->configureAppFn((UINT64) pSampleConfiguration, roleType));
    CHK_STATUS(setUpSignalingDefaults(pSampleConfiguration));
CleanUp:
    if (STATUS_FAILED(retStatus)) {
        freeSampleConfiguration(&pSampleConfiguration);
    }
    if (ppSampleConfiguration != NULL) {
        *ppSampleConfiguration = pSampleConfiguration;
    }
    return retStatus;
}

STATUS readFrameFromDisk(PBYTE pFrame, PUINT32 pSize, PCHAR frameFilePath)
{
    STATUS retStatus = STATUS_SUCCESS;
    UINT64 size = 0;
    CHK_ERR(pSize != NULL, STATUS_NULL_ARG, "[KVS Master] Invalid file size");
    size = *pSize;
    // Get the size and read into frame
    CHK_STATUS(readFile(frameFilePath, TRUE, pFrame, &size));
CleanUp:

    if (pSize != NULL) {
        *pSize = (UINT32) size;
    }

    return retStatus;
}

STATUS initializeMediaSenders(PSampleConfiguration pSampleConfiguration, startRoutine audioSource, startRoutine videoSource)
{
    STATUS retStatus = STATUS_SUCCESS;
    UINT32 frameSize;
    CHK(pSampleConfiguration != NULL && (audioSource != NULL && videoSource != NULL), STATUS_NULL_ARG);
    pSampleConfiguration->appMediaCtx.audioSource = audioSource;
    pSampleConfiguration->appMediaCtx.videoSource = videoSource;
    if (videoSource != NULL && audioSource == NULL) {
        pSampleConfiguration->appMediaCtx.mediaType = SAMPLE_STREAMING_VIDEO_ONLY;
    } else {
        pSampleConfiguration->appMediaCtx.mediaType = SAMPLE_STREAMING_AUDIO_VIDEO;
    }
    CHK_STATUS(readFrameFromDisk(NULL, &frameSize, "./h264SampleFrames/frame-0001.h264"));
    CHK_STATUS(readFrameFromDisk(NULL, &frameSize, "./opusSampleFrames/sample-001.opus"));
CleanUp:
    return retStatus;
}

STATUS initializeMediaReceivers(PSampleConfiguration pSampleConfiguration, startRoutine receiveAudioVideoSource)
{
    STATUS retStatus = STATUS_SUCCESS;
    CHK(pSampleConfiguration != NULL && receiveAudioVideoSource != NULL, STATUS_NULL_ARG);
    pSampleConfiguration->appMediaCtx.receiveAudioVideoSource = receiveAudioVideoSource;
CleanUp:
    return retStatus;
}

STATUS addTaskToTimerQueue(PSampleConfiguration pSampleConfiguration, PTimerTaskConfiguration pTimerTaskConfiguration)
{
    STATUS retStatus = STATUS_SUCCESS;
    CHK(pSampleConfiguration != NULL && pTimerTaskConfiguration != NULL, STATUS_NULL_ARG);
    CHK_LOG_ERR(timerQueueAddTimer(pSampleConfiguration->timerQueueHandle, pTimerTaskConfiguration->startTime, pTimerTaskConfiguration->iterationTime,
                                   pTimerTaskConfiguration->timerCallbackFunc, pTimerTaskConfiguration->customData,
                                   &pTimerTaskConfiguration->timerId));
    if (pTimerTaskConfiguration->timerId != MAX_UINT32) {
        CHK_STATUS(doubleListInsertItemHead(pSampleConfiguration->timerIdList, (UINT64) pTimerTaskConfiguration->timerId));
    }
CleanUp:
    return retStatus;
}

STATUS signalingCallFailed(STATUS status)
{
    return (STATUS_SIGNALING_GET_TOKEN_CALL_FAILED == status || STATUS_SIGNALING_DESCRIBE_CALL_FAILED == status ||
            STATUS_SIGNALING_CREATE_CALL_FAILED == status || STATUS_SIGNALING_GET_ENDPOINT_CALL_FAILED == status ||
            STATUS_SIGNALING_GET_ICE_CONFIG_CALL_FAILED == status || STATUS_SIGNALING_CONNECT_CALL_FAILED == status ||
            STATUS_SIGNALING_DESCRIBE_MEDIA_CALL_FAILED == status);
}

STATUS signalingClientStateChanged(UINT64 customData, SIGNALING_CLIENT_STATE state)
{
    UNUSED_PARAM(customData);
    STATUS retStatus = STATUS_SUCCESS;
    PCHAR pStateStr;

    signalingClientGetStateString(state, &pStateStr);

    DLOGV("Signaling client state changed to %d - '%s'", state, pStateStr);

    // Return success to continue
    return retStatus;
}

STATUS logSelectedIceCandidatesInformation(PSampleStreamingSession pSampleStreamingSession)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    RtcStats rtcMetrics;

    CHK(pSampleStreamingSession != NULL, STATUS_NULL_ARG);
    rtcMetrics.requestedTypeOfStats = RTC_STATS_TYPE_LOCAL_CANDIDATE;
    CHK_STATUS(rtcPeerConnectionGetMetrics(pSampleStreamingSession->pPeerConnection, NULL, &rtcMetrics));
    DLOGD("Local Candidate IP Address: %s", rtcMetrics.rtcStatsObject.localIceCandidateStats.address);
    DLOGD("Local Candidate type: %s", rtcMetrics.rtcStatsObject.localIceCandidateStats.candidateType);
    DLOGD("Local Candidate port: %d", rtcMetrics.rtcStatsObject.localIceCandidateStats.port);
    DLOGD("Local Candidate priority: %d", rtcMetrics.rtcStatsObject.localIceCandidateStats.priority);
    DLOGD("Local Candidate transport protocol: %s", rtcMetrics.rtcStatsObject.localIceCandidateStats.protocol);
    DLOGD("Local Candidate relay protocol: %s", rtcMetrics.rtcStatsObject.localIceCandidateStats.relayProtocol);
    DLOGD("Local Candidate Ice server source: %s", rtcMetrics.rtcStatsObject.localIceCandidateStats.url);

    rtcMetrics.requestedTypeOfStats = RTC_STATS_TYPE_REMOTE_CANDIDATE;
    CHK_STATUS(rtcPeerConnectionGetMetrics(pSampleStreamingSession->pPeerConnection, NULL, &rtcMetrics));
    DLOGD("Remote Candidate IP Address: %s", rtcMetrics.rtcStatsObject.remoteIceCandidateStats.address);
    DLOGD("Remote Candidate type: %s", rtcMetrics.rtcStatsObject.remoteIceCandidateStats.candidateType);
    DLOGD("Remote Candidate port: %d", rtcMetrics.rtcStatsObject.remoteIceCandidateStats.port);
    DLOGD("Remote Candidate priority: %d", rtcMetrics.rtcStatsObject.remoteIceCandidateStats.priority);
    DLOGD("Remote Candidate transport protocol: %s", rtcMetrics.rtcStatsObject.remoteIceCandidateStats.protocol);
CleanUp:
    LEAVES();
    return retStatus;
}

STATUS handleAnswer(PSampleConfiguration pSampleConfiguration, PSampleStreamingSession pSampleStreamingSession, PSignalingMessage pSignalingMessage)
{
    UNUSED_PARAM(pSampleConfiguration);
    STATUS retStatus = STATUS_SUCCESS;
    RtcSessionDescriptionInit answerSessionDescriptionInit;

    MEMSET(&answerSessionDescriptionInit, 0x00, SIZEOF(RtcSessionDescriptionInit));

    CHK_STATUS(deserializeSessionDescriptionInit(pSignalingMessage->payload, pSignalingMessage->payloadLen, &answerSessionDescriptionInit));
    CHK_STATUS(setRemoteDescription(pSampleStreamingSession->pPeerConnection, &answerSessionDescriptionInit));

CleanUp:

    CHK_LOG_ERR(retStatus);

    return retStatus;
}

PVOID mediaSenderRoutine(PVOID customData)
{
    STATUS retStatus = STATUS_SUCCESS;
    PSampleConfiguration pSampleConfiguration = (PSampleConfiguration) customData;
    CHK(pSampleConfiguration != NULL, STATUS_NULL_ARG);
    pSampleConfiguration->appMediaCtx.videoSenderTid = INVALID_TID_VALUE;
    pSampleConfiguration->appMediaCtx.audioSenderTid = INVALID_TID_VALUE;

    MUTEX_LOCK(pSampleConfiguration->sampleConfigurationObjLock);
    while (!ATOMIC_LOAD_BOOL(&pSampleConfiguration->connected) && !ATOMIC_LOAD_BOOL(&pSampleConfiguration->appTerminateFlag)) {
        CVAR_WAIT(pSampleConfiguration->cvar, pSampleConfiguration->sampleConfigurationObjLock, 5 * HUNDREDS_OF_NANOS_IN_A_SECOND);
    }
    MUTEX_UNLOCK(pSampleConfiguration->sampleConfigurationObjLock);

    CHK(!ATOMIC_LOAD_BOOL(&pSampleConfiguration->appTerminateFlag), retStatus);

    if (pSampleConfiguration->appMediaCtx.videoSource != NULL) {
        THREAD_CREATE(&pSampleConfiguration->appMediaCtx.videoSenderTid, pSampleConfiguration->appMediaCtx.videoSource, (PVOID) pSampleConfiguration);
    }

    if (pSampleConfiguration->appMediaCtx.audioSource != NULL) {
        THREAD_CREATE(&pSampleConfiguration->appMediaCtx.audioSenderTid, pSampleConfiguration->appMediaCtx.audioSource, (PVOID) pSampleConfiguration);
    }

    if (pSampleConfiguration->appMediaCtx.videoSenderTid != INVALID_TID_VALUE) {
        THREAD_JOIN(pSampleConfiguration->appMediaCtx.videoSenderTid, NULL);
    }

    if (pSampleConfiguration->appMediaCtx.audioSenderTid != INVALID_TID_VALUE) {
        THREAD_JOIN(pSampleConfiguration->appMediaCtx.audioSenderTid, NULL);
    }

CleanUp:
    // clean the flag of the media thread.
    ATOMIC_STORE_BOOL(&pSampleConfiguration->mediaThreadStarted, FALSE);
    CHK_LOG_ERR(retStatus);
    return NULL;
}

STATUS handleOffer(PSampleConfiguration pSampleConfiguration, PSampleStreamingSession pSampleStreamingSession, PSignalingMessage pSignalingMessage)
{
    STATUS retStatus = STATUS_SUCCESS;
    RtcSessionDescriptionInit offerSessionDescriptionInit;
    NullableBool canTrickle;
    BOOL mediaThreadStarted;

    CHK(pSampleConfiguration != NULL && pSignalingMessage != NULL, STATUS_NULL_ARG);

    MEMSET(&offerSessionDescriptionInit, 0x00, SIZEOF(RtcSessionDescriptionInit));
    MEMSET(&pSampleStreamingSession->answerSessionDescriptionInit, 0x00, SIZEOF(RtcSessionDescriptionInit));
    CHK_STATUS(deserializeSessionDescriptionInit(pSignalingMessage->payload, pSignalingMessage->payloadLen, &offerSessionDescriptionInit));
    CHK_STATUS(setRemoteDescription(pSampleStreamingSession->pPeerConnection, &offerSessionDescriptionInit));
    canTrickle = canTrickleIceCandidates(pSampleStreamingSession->pPeerConnection);
    /* cannot be null after setRemoteDescription */
    CHECK(!NULLABLE_CHECK_EMPTY(canTrickle));
    pSampleStreamingSession->remoteCanTrickleIce = canTrickle.value;
    CHK_STATUS(setLocalDescription(pSampleStreamingSession->pPeerConnection, &pSampleStreamingSession->answerSessionDescriptionInit));

    /*
     * If remote support trickle ice, send answer now. Otherwise answer will be sent once ice candidate gathering is complete.
     */
    if (pSampleStreamingSession->remoteCanTrickleIce) {
        CHK_STATUS(createAnswer(pSampleStreamingSession->pPeerConnection, &pSampleStreamingSession->answerSessionDescriptionInit));
        CHK_STATUS(respondWithAnswer(pSampleStreamingSession));
    }

    mediaThreadStarted = ATOMIC_EXCHANGE_BOOL(&pSampleConfiguration->mediaThreadStarted, TRUE);
    if (!mediaThreadStarted) {
        THREAD_CREATE(&pSampleConfiguration->appMediaCtx.mediaSenderTid, mediaSenderRoutine, (PVOID) pSampleConfiguration);
    }

    // The audio video receive routine should be per streaming session
    if (pSampleConfiguration->appMediaCtx.receiveAudioVideoSource != NULL) {
        THREAD_CREATE(&pSampleStreamingSession->receiveAudioVideoSenderTid, pSampleConfiguration->appMediaCtx.receiveAudioVideoSource,
                      (PVOID) pSampleStreamingSession);
    }
CleanUp:

    CHK_LOG_ERR(retStatus);

    return retStatus;
}

STATUS sendSignalingMessage(PSampleStreamingSession pSampleStreamingSession, PSignalingMessage pMessage)
{
    STATUS retStatus = STATUS_SUCCESS;
    BOOL locked = FALSE;
    PSampleConfiguration pSampleConfiguration;
    // Validate the input params
    CHK(pSampleStreamingSession != NULL && pSampleStreamingSession->pSampleConfiguration != NULL && pMessage != NULL, STATUS_NULL_ARG);

    pSampleConfiguration = pSampleStreamingSession->pSampleConfiguration;

    CHK(IS_VALID_MUTEX_VALUE(pSampleConfiguration->appSignalingCtx.signalingSendMessageLock) &&
            IS_VALID_SIGNALING_CLIENT_HANDLE(pSampleConfiguration->appSignalingCtx.signalingClientHandle),
        STATUS_INVALID_OPERATION);

    MUTEX_LOCK(pSampleConfiguration->appSignalingCtx.signalingSendMessageLock);
    locked = TRUE;
    CHK_STATUS(signalingClientSendMessageSync(pSampleConfiguration->appSignalingCtx.signalingClientHandle, pMessage));
    if (pMessage->messageType == SIGNALING_MESSAGE_TYPE_ANSWER) {
        CHK_STATUS(signalingClientGetMetrics(pSampleConfiguration->appSignalingCtx.signalingClientHandle, &pSampleConfiguration->appSignalingCtx.signalingClientMetrics));
        DLOGP("[Signaling offer received to answer sent time] %" PRIu64 " ms",
              pSampleConfiguration->appSignalingCtx.signalingClientMetrics.signalingClientStats.offerToAnswerTime);
    }

CleanUp:

    if (locked) {
        MUTEX_UNLOCK(pSampleStreamingSession->pSampleConfiguration->appSignalingCtx.signalingSendMessageLock);
    }

    CHK_LOG_ERR(retStatus);
    return retStatus;
}

STATUS respondWithAnswer(PSampleStreamingSession pSampleStreamingSession)
{
    STATUS retStatus = STATUS_SUCCESS;
    SignalingMessage message;
    UINT32 buffLen = MAX_SIGNALING_MESSAGE_LEN;

    CHK_STATUS(serializeSessionDescriptionInit(&pSampleStreamingSession->answerSessionDescriptionInit, message.payload, &buffLen));

    message.version = SIGNALING_MESSAGE_CURRENT_VERSION;
    message.messageType = SIGNALING_MESSAGE_TYPE_ANSWER;
    STRNCPY(message.peerClientId, pSampleStreamingSession->peerId, MAX_SIGNALING_CLIENT_ID_LEN);
    message.payloadLen = (UINT32) STRLEN(message.payload);
    // SNPRINTF appends null terminator, so we do not manually add it
    SNPRINTF(message.correlationId, MAX_CORRELATION_ID_LEN, "%llu_%llu", GETTIME(), ATOMIC_INCREMENT(&pSampleStreamingSession->correlationIdPostFix));
    DLOGD("Responding With Answer With correlationId: %s", message.correlationId);
    CHK_STATUS(sendSignalingMessage(pSampleStreamingSession, &message));

CleanUp:

    CHK_LOG_ERR(retStatus);
    return retStatus;
}

PVOID asyncGetIceConfigInfo(PVOID args)
{
    STATUS retStatus = STATUS_SUCCESS;
    AsyncGetIceStruct* data = (AsyncGetIceStruct*) args;
    PIceConfigInfo pIceConfigInfo = NULL;
    UINT32 uriCount = 0;
    UINT32 i = 0, maxTurnServer = 1;

    if (data != NULL) {
        /* signalingClientGetIceConfigInfoCount can return more than one turn server. Use only one to optimize
         * candidate gathering latency. But user can also choose to use more than 1 turn server. */
        for (uriCount = 0, i = 0; i < maxTurnServer; i++) {
            /*
             * if configuration.iceServers[uriCount + 1].urls is "turn:ip:port?transport=udp" then ICE will try TURN over UDP
             * if configuration.iceServers[uriCount + 1].urls is "turn:ip:port?transport=tcp" then ICE will try TURN over TCP/TLS
             * if configuration.iceServers[uriCount + 1].urls is "turns:ip:port?transport=udp", it's currently ignored because sdk dont do TURN
             * over DTLS yet. if configuration.iceServers[uriCount + 1].urls is "turns:ip:port?transport=tcp" then ICE will try TURN over TCP/TLS
             * if configuration.iceServers[uriCount + 1].urls is "turn:ip:port" then ICE will try both TURN over UDP and TCP/TLS
             *
             * It's recommended to not pass too many TURN iceServers to configuration because it will slow down ice gathering in non-trickle mode.
             */
            CHK_STATUS(signalingClientGetIceConfigInfo(data->signalingClientHandle, i, &pIceConfigInfo));
            CHECK(uriCount < MAX_ICE_SERVERS_COUNT);
            uriCount += pIceConfigInfo->uriCount;
            CHK_STATUS(addConfigToServerList(&(data->pRtcPeerConnection), pIceConfigInfo));
        }
    }
    *(data->pUriCount) += uriCount;

CleanUp:
    SAFE_MEMFREE(data);
    CHK_LOG_ERR(retStatus);
    return NULL;
}

STATUS initializePeerConnection(PSampleConfiguration pSampleConfiguration, PRtcPeerConnection* ppRtcPeerConnection)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    RtcConfiguration configuration;
#ifndef ENABLE_KVS_THREADPOOL
    UINT32 i, j, maxTurnServer = 1;
    PIceConfigInfo pIceConfigInfo;
    UINT32 uriCount = 0;
#endif
    UINT64 data;
    PRtcCertificate pRtcCertificate = NULL;

    CHK(pSampleConfiguration != NULL && ppRtcPeerConnection != NULL, STATUS_NULL_ARG);

    MEMSET(&configuration, 0x00, SIZEOF(RtcConfiguration));

    // Set this to custom callback to enable filtering of interfaces
    configuration.kvsRtcConfiguration.iceSetInterfaceFilterFunc = NULL;

    // Set the ICE mode explicitly
    configuration.iceTransportPolicy = ICE_TRANSPORT_POLICY_ALL;

    // Set the  STUN server
    PCHAR pKinesisVideoStunUrlPostFix = KINESIS_VIDEO_STUN_URL_POSTFIX;
    // If region is in CN, add CN region uri postfix
    if (STRSTR(pSampleConfiguration->appSignalingCtx.channelInfo.pRegion, "cn-")) {
        pKinesisVideoStunUrlPostFix = KINESIS_VIDEO_STUN_URL_POSTFIX_CN;
    }
    SNPRINTF(configuration.iceServers[0].urls, MAX_ICE_CONFIG_URI_LEN, KINESIS_VIDEO_STUN_URL, pSampleConfiguration->appSignalingCtx.channelInfo.pRegion,
             pKinesisVideoStunUrlPostFix);

    // Check if we have any pregenerated certs and use them
    // NOTE: We are running under the config lock
    retStatus = stackQueueDequeue(pSampleConfiguration->pregeneratedCertificates, &data);
    CHK(retStatus == STATUS_SUCCESS || retStatus == STATUS_NOT_FOUND, retStatus);

    if (retStatus == STATUS_NOT_FOUND) {
        retStatus = STATUS_SUCCESS;
    } else {
        // Use the pre-generated cert and get rid of it to not reuse again
        pRtcCertificate = (PRtcCertificate) data;
        configuration.certificates[0] = *pRtcCertificate;
    }

    CHK_STATUS(createPeerConnection(&configuration, ppRtcPeerConnection));

    if (pSampleConfiguration->appConfigCtx.useTurn) {
#ifdef ENABLE_KVS_THREADPOOL
        pSampleConfiguration->iceUriCount = 1;
        AsyncGetIceStruct* pAsyncData = NULL;

        pAsyncData = (AsyncGetIceStruct*) MEMCALLOC(1, SIZEOF(AsyncGetIceStruct));
        pAsyncData->signalingClientHandle = pSampleConfiguration->appSignalingCtx.signalingClientHandle;
        pAsyncData->pRtcPeerConnection = *ppRtcPeerConnection;
        pAsyncData->pUriCount = &(pSampleConfiguration->iceUriCount);
        CHK_STATUS(peerConnectionAsync(asyncGetIceConfigInfo, (PVOID) pAsyncData));
#else

        /* signalingClientGetIceConfigInfoCount can return more than one turn server. Use only one to optimize
         * candidate gathering latency. But user can also choose to use more than 1 turn server. */
        for (uriCount = 0, i = 0; i < maxTurnServer; i++) {
            /*
             * if configuration.iceServers[uriCount + 1].urls is "turn:ip:port?transport=udp" then ICE will try TURN over UDP
             * if configuration.iceServers[uriCount + 1].urls is "turn:ip:port?transport=tcp" then ICE will try TURN over TCP/TLS
             * if configuration.iceServers[uriCount + 1].urls is "turns:ip:port?transport=udp", it's currently ignored because sdk dont do TURN
             * over DTLS yet. if configuration.iceServers[uriCount + 1].urls is "turns:ip:port?transport=tcp" then ICE will try TURN over TCP/TLS
             * if configuration.iceServers[uriCount + 1].urls is "turn:ip:port" then ICE will try both TURN over UDP and TCP/TLS
             *
             * It's recommended to not pass too many TURN iceServers to configuration because it will slow down ice gathering in non-trickle mode.
             */
            CHK_STATUS(signalingClientGetIceConfigInfo(pSampleConfiguration->signalingClientHandle, i, &pIceConfigInfo));
            CHECK(uriCount < MAX_ICE_SERVERS_COUNT);
            uriCount += pIceConfigInfo->uriCount;
            CHK_STATUS(addConfigToServerList(ppRtcPeerConnection, pIceConfigInfo));
        }
        pSampleConfiguration->iceUriCount = uriCount + 1;
#endif
    }

CleanUp:

    CHK_LOG_ERR(retStatus);

    // Free the certificate which can be NULL as we no longer need it and won't reuse
    freeRtcCertificate(pRtcCertificate);

    LEAVES();
    return retStatus;
}

// Return ICE server stats for a specific streaming session
STATUS gatherIceServerStats(PSampleStreamingSession pSampleStreamingSession)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    RtcStats rtcmetrics;
    UINT32 j = 0;
    rtcmetrics.requestedTypeOfStats = RTC_STATS_TYPE_ICE_SERVER;
    for (; j < pSampleStreamingSession->pSampleConfiguration->iceUriCount; j++) {
        rtcmetrics.rtcStatsObject.iceServerStats.iceServerIndex = j;
        CHK_STATUS(rtcPeerConnectionGetMetrics(pSampleStreamingSession->pPeerConnection, NULL, &rtcmetrics));
        DLOGD("ICE Server URL: %s", rtcmetrics.rtcStatsObject.iceServerStats.url);
        DLOGD("ICE Server port: %d", rtcmetrics.rtcStatsObject.iceServerStats.port);
        DLOGD("ICE Server protocol: %s", rtcmetrics.rtcStatsObject.iceServerStats.protocol);
        DLOGD("Total requests sent:%" PRIu64, rtcmetrics.rtcStatsObject.iceServerStats.totalRequestsSent);
        DLOGD("Total responses received: %" PRIu64, rtcmetrics.rtcStatsObject.iceServerStats.totalResponsesReceived);
        DLOGD("Total round trip time: %" PRIu64 "ms",
              rtcmetrics.rtcStatsObject.iceServerStats.totalRoundTripTime / HUNDREDS_OF_NANOS_IN_A_MILLISECOND);
    }
CleanUp:
    LEAVES();
    return retStatus;
}

VOID sampleVideoFrameHandler(UINT64 customData, PFrame pFrame)
{
    UNUSED_PARAM(customData);
    DLOGV("Video Frame received. TrackId: %" PRIu64 ", Size: %u, Flags %u", pFrame->trackId, pFrame->size, pFrame->flags);
}

VOID sampleAudioFrameHandler(UINT64 customData, PFrame pFrame)
{
    UNUSED_PARAM(customData);
    DLOGV("Audio Frame received. TrackId: %" PRIu64 ", Size: %u, Flags %u", pFrame->trackId, pFrame->size, pFrame->flags);
}

VOID sampleFrameHandler(UINT64 customData, PFrame pFrame)
{
    UNUSED_PARAM(customData);
    DLOGV("Video Frame received. TrackId: %" PRIu64 ", Size: %u, Flags %u", pFrame->trackId, pFrame->size, pFrame->flags);
}

VOID sampleBandwidthEstimationHandler(UINT64 customData, DOUBLE maximumBitrate)
{
    UNUSED_PARAM(customData);
    DLOGV("received bitrate suggestion: %f", maximumBitrate);
}

VOID sampleSenderBandwidthEstimationHandler(UINT64 customData, UINT32 txBytes, UINT32 rxBytes, UINT32 txPacketsCnt, UINT32 rxPacketsCnt,
                                            UINT64 duration)
{
    UNUSED_PARAM(customData);
    UNUSED_PARAM(duration);
    UNUSED_PARAM(rxBytes);
    UNUSED_PARAM(txBytes);
    UINT32 lostPacketsCnt = txPacketsCnt - rxPacketsCnt;
    UINT32 percentLost = lostPacketsCnt * 100 / txPacketsCnt;
    UINT32 bitrate = 1024;
    if (percentLost < 2) {
        // increase encoder bitrate by 2 percent
        bitrate *= 1.02f;
    } else if (percentLost > 5) {
        // decrease encoder bitrate by packet loss percent
        bitrate *= (1.0f - percentLost / 100.0f);
    }
    // otherwise keep bitrate the same

    DLOGS("received sender bitrate estimation: suggested bitrate %u sent: %u bytes %u packets received: %u bytes %u packets in %lu msec, ", bitrate,
          txBytes, txPacketsCnt, rxBytes, rxPacketsCnt, duration / 10000ULL);
}

STATUS handleRemoteCandidate(PSampleStreamingSession pSampleStreamingSession, PSignalingMessage pSignalingMessage)
{
    STATUS retStatus = STATUS_SUCCESS;
    RtcIceCandidateInit iceCandidate;
    CHK(pSampleStreamingSession != NULL && pSignalingMessage != NULL, STATUS_NULL_ARG);

    CHK_STATUS(deserializeRtcIceCandidateInit(pSignalingMessage->payload, pSignalingMessage->payloadLen, &iceCandidate));
    CHK_STATUS(addIceCandidate(pSampleStreamingSession->pPeerConnection, iceCandidate.candidate));

CleanUp:

    CHK_LOG_ERR(retStatus);
    return retStatus;
}

STATUS traverseDirectoryPEMFileScan(UINT64 customData, DIR_ENTRY_TYPES entryType, PCHAR fullPath, PCHAR fileName)
{
    UNUSED_PARAM(entryType);
    UNUSED_PARAM(fullPath);

    PCHAR certName = (PCHAR) customData;
    UINT32 fileNameLen = STRLEN(fileName);

    if (fileNameLen > ARRAY_SIZE(CA_CERT_PEM_FILE_EXTENSION) + 1 &&
        (STRCMPI(CA_CERT_PEM_FILE_EXTENSION, &fileName[fileNameLen - ARRAY_SIZE(CA_CERT_PEM_FILE_EXTENSION) + 1]) == 0)) {
        certName[0] = FPATHSEPARATOR;
        certName++;
        STRCPY(certName, fileName);
    }

    return STATUS_SUCCESS;
}

STATUS lookForSslCert(PSampleConfiguration* ppSampleConfiguration)
{
    STATUS retStatus = STATUS_SUCCESS;
    struct stat pathStat;
    CHAR certName[MAX_PATH_LEN];
    PSampleConfiguration pSampleConfiguration = *ppSampleConfiguration;

    MEMSET(certName, 0x0, ARRAY_SIZE(certName));
    pSampleConfiguration->pCaCertPath = GETENV(CACERT_PATH_ENV_VAR);

    // if ca cert path is not set from the environment, try to use the one that cmake detected
    if (pSampleConfiguration->pCaCertPath == NULL) {
        CHK_ERR(STRNLEN(DEFAULT_KVS_CACERT_PATH, MAX_PATH_LEN) > 0, STATUS_INVALID_OPERATION, "No ca cert path given (error:%s)", strerror(errno));
        pSampleConfiguration->pCaCertPath = DEFAULT_KVS_CACERT_PATH;
    } else {
        // Check if the environment variable is a path
        CHK(0 == FSTAT(pSampleConfiguration->pCaCertPath, &pathStat), STATUS_DIRECTORY_ENTRY_STAT_ERROR);

        if (S_ISDIR(pathStat.st_mode)) {
            CHK_STATUS(traverseDirectory(pSampleConfiguration->pCaCertPath, (UINT64) &certName, /* iterate */ FALSE, traverseDirectoryPEMFileScan));

            if (certName[0] != 0x0) {
                STRCAT(pSampleConfiguration->pCaCertPath, certName);
            } else {
                DLOGW("Cert not found in path set...checking if CMake detected a path\n");
                CHK_ERR(STRNLEN(DEFAULT_KVS_CACERT_PATH, MAX_PATH_LEN) > 0, STATUS_INVALID_OPERATION, "No ca cert path given (error:%s)",
                        strerror(errno));
                DLOGI("CMake detected cert path\n");
                pSampleConfiguration->pCaCertPath = DEFAULT_KVS_CACERT_PATH;
            }
        }
    }

CleanUp:

    CHK_LOG_ERR(retStatus);
    return retStatus;
}

STATUS initSignaling(PSampleConfiguration pSampleConfiguration, PCHAR clientId)
{
    STATUS retStatus = STATUS_SUCCESS;
    SignalingClientMetrics signalingClientMetrics = pSampleConfiguration->appSignalingCtx.signalingClientMetrics;
    pSampleConfiguration->appSignalingCtx.signalingClientCallbacks.messageReceivedFn = signalingMessageReceived;
    STRCPY(pSampleConfiguration->appSignalingCtx.clientInfo.clientId, clientId);
    CHK_STATUS(createSignalingClientSync(&pSampleConfiguration->appSignalingCtx.clientInfo, &pSampleConfiguration->appSignalingCtx.channelInfo,
                                         &pSampleConfiguration->appSignalingCtx.signalingClientCallbacks, pSampleConfiguration->pCredentialProvider,
                                         &pSampleConfiguration->appSignalingCtx.signalingClientHandle));

    // Enable the processing of the messages
    CHK_STATUS(signalingClientFetchSync(pSampleConfiguration->appSignalingCtx.signalingClientHandle));

    CHK_STATUS(signalingClientConnectSync(pSampleConfiguration->appSignalingCtx.signalingClientHandle));

    signalingClientGetMetrics(pSampleConfiguration->appSignalingCtx.signalingClientHandle, &signalingClientMetrics);

    // Logging this here since the logs in signaling library do not get routed to file
    DLOGP("[Signaling Get token] %" PRIu64 " ms", signalingClientMetrics.signalingClientStats.getTokenCallTime);
    DLOGP("[Signaling Describe] %" PRIu64 " ms", signalingClientMetrics.signalingClientStats.describeCallTime);
    DLOGP("[Signaling Describe Media] %" PRIu64 " ms", signalingClientMetrics.signalingClientStats.describeMediaCallTime);
    DLOGP("[Signaling Create Channel] %" PRIu64 " ms", signalingClientMetrics.signalingClientStats.createCallTime);
    DLOGP("[Signaling Get endpoint] %" PRIu64 " ms", signalingClientMetrics.signalingClientStats.getEndpointCallTime);
    DLOGP("[Signaling Get ICE config] %" PRIu64 " ms", signalingClientMetrics.signalingClientStats.getIceConfigCallTime);
    DLOGP("[Signaling Connect] %" PRIu64 " ms", signalingClientMetrics.signalingClientStats.connectCallTime);
    if (signalingClientMetrics.signalingClientStats.joinSessionCallTime != 0) {
        DLOGP("[Signaling Join Session] %" PRIu64 " ms", signalingClientMetrics.signalingClientStats.joinSessionCallTime);
    }
    DLOGP("[Signaling create client] %" PRIu64 " ms", signalingClientMetrics.signalingClientStats.createClientTime);
    DLOGP("[Signaling fetch client] %" PRIu64 " ms", signalingClientMetrics.signalingClientStats.fetchClientTime);
    DLOGP("[Signaling connect client] %" PRIu64 " ms", signalingClientMetrics.signalingClientStats.connectClientTime);
    pSampleConfiguration->appSignalingCtx.signalingClientMetrics = signalingClientMetrics;
    gSampleConfiguration = pSampleConfiguration;
CleanUp:
    return retStatus;
}

STATUS logSignalingClientStats(PSignalingClientMetrics pSignalingClientMetrics)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    CHK(pSignalingClientMetrics != NULL, STATUS_NULL_ARG);
    DLOGD("Signaling client connection duration: %" PRIu64 " ms",
          (pSignalingClientMetrics->signalingClientStats.connectionDuration / HUNDREDS_OF_NANOS_IN_A_MILLISECOND));
    DLOGD("Number of signaling client API errors: %d", pSignalingClientMetrics->signalingClientStats.numberOfErrors);
    DLOGD("Number of runtime errors in the session: %d", pSignalingClientMetrics->signalingClientStats.numberOfRuntimeErrors);
    DLOGD("Signaling client uptime: %" PRIu64 " ms",
          (pSignalingClientMetrics->signalingClientStats.connectionDuration / HUNDREDS_OF_NANOS_IN_A_MILLISECOND));
    // This gives the EMA of the createChannel, describeChannel, getChannelEndpoint and deleteChannel calls
    DLOGD("Control Plane API call latency: %" PRIu64 " ms",
          (pSignalingClientMetrics->signalingClientStats.cpApiCallLatency / HUNDREDS_OF_NANOS_IN_A_MILLISECOND));
    // This gives the EMA of the getIceConfig() call.
    DLOGD("Data Plane API call latency: %" PRIu64 " ms",
          (pSignalingClientMetrics->signalingClientStats.dpApiCallLatency / HUNDREDS_OF_NANOS_IN_A_MILLISECOND));
    DLOGD("API call retry count: %d", pSignalingClientMetrics->signalingClientStats.apiCallRetryCount);
CleanUp:
    LEAVES();
    return retStatus;
}

STATUS getIceCandidatePairStatsCallback(UINT32 timerId, UINT64 currentTime, UINT64 customData)
{
    UNUSED_PARAM(timerId);
    UNUSED_PARAM(currentTime);
    STATUS retStatus = STATUS_SUCCESS;
    PSampleConfiguration pSampleConfiguration = (PSampleConfiguration) customData;
    UINT32 i;
    UINT64 currentMeasureDuration = 0;
    DOUBLE averagePacketsDiscardedOnSend = 0.0;
    DOUBLE averageNumberOfPacketsSentPerSecond = 0.0;
    DOUBLE averageNumberOfPacketsReceivedPerSecond = 0.0;
    DOUBLE outgoingBitrate = 0.0;
    DOUBLE incomingBitrate = 0.0;
    BOOL locked = FALSE;

    CHK_WARN(pSampleConfiguration != NULL, STATUS_NULL_ARG, "[KVS Master] getPeriodicStats(): Passed argument is NULL");

    pSampleConfiguration->rtcIceCandidatePairMetrics.requestedTypeOfStats = RTC_STATS_TYPE_CANDIDATE_PAIR;

    // Use MUTEX_TRYLOCK to avoid possible dead lock when canceling timerQueue
    if (!MUTEX_TRYLOCK(pSampleConfiguration->sampleConfigurationObjLock)) {
        return retStatus;
    } else {
        locked = TRUE;
    }

    for (i = 0; i < pSampleConfiguration->streamingSessionCount; ++i) {
        if (STATUS_SUCCEEDED(rtcPeerConnectionGetMetrics(pSampleConfiguration->sampleStreamingSessionList[i]->pPeerConnection, NULL,
                                                         &pSampleConfiguration->rtcIceCandidatePairMetrics))) {
            currentMeasureDuration = (pSampleConfiguration->rtcIceCandidatePairMetrics.timestamp -
                                      pSampleConfiguration->sampleStreamingSessionList[i]->rtcMetricsHistory.prevTs) /
                HUNDREDS_OF_NANOS_IN_A_SECOND;
            DLOGD("Current duration: %" PRIu64 " seconds", currentMeasureDuration);
            if (currentMeasureDuration > 0) {
                DLOGD("Selected local candidate ID: %s",
                      pSampleConfiguration->rtcIceCandidatePairMetrics.rtcStatsObject.iceCandidatePairStats.localCandidateId);
                DLOGD("Selected remote candidate ID: %s",
                      pSampleConfiguration->rtcIceCandidatePairMetrics.rtcStatsObject.iceCandidatePairStats.remoteCandidateId);
                // TODO: Display state as a string for readability
                DLOGD("Ice Candidate Pair state: %d", pSampleConfiguration->rtcIceCandidatePairMetrics.rtcStatsObject.iceCandidatePairStats.state);
                DLOGD("Nomination state: %s",
                      pSampleConfiguration->rtcIceCandidatePairMetrics.rtcStatsObject.iceCandidatePairStats.nominated ? "nominated"
                                                                                                                      : "not nominated");
                averageNumberOfPacketsSentPerSecond =
                    (DOUBLE) (pSampleConfiguration->rtcIceCandidatePairMetrics.rtcStatsObject.iceCandidatePairStats.packetsSent -
                              pSampleConfiguration->sampleStreamingSessionList[i]->rtcMetricsHistory.prevNumberOfPacketsSent) /
                    (DOUBLE) currentMeasureDuration;
                DLOGD("Packet send rate: %lf pkts/sec", averageNumberOfPacketsSentPerSecond);

                averageNumberOfPacketsReceivedPerSecond =
                    (DOUBLE) (pSampleConfiguration->rtcIceCandidatePairMetrics.rtcStatsObject.iceCandidatePairStats.packetsReceived -
                              pSampleConfiguration->sampleStreamingSessionList[i]->rtcMetricsHistory.prevNumberOfPacketsReceived) /
                    (DOUBLE) currentMeasureDuration;
                DLOGD("Packet receive rate: %lf pkts/sec", averageNumberOfPacketsReceivedPerSecond);

                outgoingBitrate = (DOUBLE) ((pSampleConfiguration->rtcIceCandidatePairMetrics.rtcStatsObject.iceCandidatePairStats.bytesSent -
                                             pSampleConfiguration->sampleStreamingSessionList[i]->rtcMetricsHistory.prevNumberOfBytesSent) *
                                            8.0) /
                    currentMeasureDuration;
                DLOGD("Outgoing bit rate: %lf bps", outgoingBitrate);

                incomingBitrate = (DOUBLE) ((pSampleConfiguration->rtcIceCandidatePairMetrics.rtcStatsObject.iceCandidatePairStats.bytesReceived -
                                             pSampleConfiguration->sampleStreamingSessionList[i]->rtcMetricsHistory.prevNumberOfBytesReceived) *
                                            8.0) /
                    currentMeasureDuration;
                DLOGD("Incoming bit rate: %lf bps", incomingBitrate);

                averagePacketsDiscardedOnSend =
                    (DOUBLE) (pSampleConfiguration->rtcIceCandidatePairMetrics.rtcStatsObject.iceCandidatePairStats.packetsDiscardedOnSend -
                              pSampleConfiguration->sampleStreamingSessionList[i]->rtcMetricsHistory.prevPacketsDiscardedOnSend) /
                    (DOUBLE) currentMeasureDuration;
                DLOGD("Packet discard rate: %lf pkts/sec", averagePacketsDiscardedOnSend);

                DLOGD("Current STUN request round trip time: %lf sec",
                      pSampleConfiguration->rtcIceCandidatePairMetrics.rtcStatsObject.iceCandidatePairStats.currentRoundTripTime);
                DLOGD("Number of STUN responses received: %llu",
                      pSampleConfiguration->rtcIceCandidatePairMetrics.rtcStatsObject.iceCandidatePairStats.responsesReceived);

                pSampleConfiguration->sampleStreamingSessionList[i]->rtcMetricsHistory.prevTs =
                    pSampleConfiguration->rtcIceCandidatePairMetrics.timestamp;
                pSampleConfiguration->sampleStreamingSessionList[i]->rtcMetricsHistory.prevNumberOfPacketsSent =
                    pSampleConfiguration->rtcIceCandidatePairMetrics.rtcStatsObject.iceCandidatePairStats.packetsSent;
                pSampleConfiguration->sampleStreamingSessionList[i]->rtcMetricsHistory.prevNumberOfPacketsReceived =
                    pSampleConfiguration->rtcIceCandidatePairMetrics.rtcStatsObject.iceCandidatePairStats.packetsReceived;
                pSampleConfiguration->sampleStreamingSessionList[i]->rtcMetricsHistory.prevNumberOfBytesSent =
                    pSampleConfiguration->rtcIceCandidatePairMetrics.rtcStatsObject.iceCandidatePairStats.bytesSent;
                pSampleConfiguration->sampleStreamingSessionList[i]->rtcMetricsHistory.prevNumberOfBytesReceived =
                    pSampleConfiguration->rtcIceCandidatePairMetrics.rtcStatsObject.iceCandidatePairStats.bytesReceived;
                pSampleConfiguration->sampleStreamingSessionList[i]->rtcMetricsHistory.prevPacketsDiscardedOnSend =
                    pSampleConfiguration->rtcIceCandidatePairMetrics.rtcStatsObject.iceCandidatePairStats.packetsDiscardedOnSend;
            }
        }
    }

CleanUp:

    if (locked) {
        MUTEX_UNLOCK(pSampleConfiguration->sampleConfigurationObjLock);
    }

    return retStatus;
}

STATUS pregenerateCertTimerCallback(UINT32 timerId, UINT64 currentTime, UINT64 customData)
{
    UNUSED_PARAM(timerId);
    UNUSED_PARAM(currentTime);
    STATUS retStatus = STATUS_SUCCESS;
    PSampleConfiguration pSampleConfiguration = (PSampleConfiguration) customData;
    BOOL locked = FALSE;
    UINT32 certCount;
    PRtcCertificate pRtcCertificate = NULL;

    CHK_WARN(pSampleConfiguration != NULL, STATUS_NULL_ARG, "[KVS Master] pregenerateCertTimerCallback(): Passed argument is NULL");

    // Use MUTEX_TRYLOCK to avoid possible dead lock when canceling timerQueue
    if (!MUTEX_TRYLOCK(pSampleConfiguration->sampleConfigurationObjLock)) {
        return retStatus;
    } else {
        locked = TRUE;
    }

    // Quick check if there is anything that needs to be done.
    CHK_STATUS(stackQueueGetCount(pSampleConfiguration->pregeneratedCertificates, &certCount));
    CHK(certCount != MAX_RTCCONFIGURATION_CERTIFICATES, retStatus);

    // Generate the certificate with the keypair
    CHK_STATUS(createRtcCertificate(&pRtcCertificate));

    // Add to the stack queue
    CHK_STATUS(stackQueueEnqueue(pSampleConfiguration->pregeneratedCertificates, (UINT64) pRtcCertificate));

    DLOGV("New certificate has been pre-generated and added to the queue");

    // Reset it so it won't be freed on exit
    pRtcCertificate = NULL;

    MUTEX_UNLOCK(pSampleConfiguration->sampleConfigurationObjLock);
    locked = FALSE;

CleanUp:

    if (pRtcCertificate != NULL) {
        freeRtcCertificate(pRtcCertificate);
    }

    if (locked) {
        MUTEX_UNLOCK(pSampleConfiguration->sampleConfigurationObjLock);
    }

    return retStatus;
}

static STATUS cancelTimerTasks(PSampleConfiguration pSampleConfiguration)
{
    STATUS retStatus = STATUS_SUCCESS;
    PDoubleListNode pCurNode = NULL;
    UINT64 timerId;
    CHK_STATUS(doubleListGetHeadNode(pSampleConfiguration->timerIdList, &pCurNode));
    while (pCurNode != NULL) {
        CHK_STATUS(doubleListGetNodeData(pCurNode, &timerId));
        pCurNode = pCurNode->pNext;
        CHK_STATUS(timerQueueCancelTimer(pSampleConfiguration->timerQueueHandle, timerId, (UINT64) pSampleConfiguration));
    }
    CHK_STATUS(doubleListClear(pSampleConfiguration->timerIdList, FALSE));
CleanUp:
    return retStatus;
}

static STATUS freeDemoCredentialProvider(PSampleConfiguration pSampleConfiguration)
{
    STATUS retStatus = STATUS_SUCCESS;
#ifdef IOT_CORE_ENABLE_CREDENTIALS
    CHK_LOG_ERR(freeIotCredentialProvider(&pSampleConfiguration->pCredentialProvider));
#else
    CHK_LOG_ERR(freeStaticCredentialProvider(&pSampleConfiguration->pCredentialProvider));
#endif
CleanUp:
    return retStatus;
}

STATUS freeSampleConfiguration(PSampleConfiguration* ppSampleConfiguration)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PSampleConfiguration pSampleConfiguration;
    UINT32 i;
    UINT64 data;
    StackQueueIterator iterator;
    BOOL locked = FALSE;
    SignalingClientMetrics signalingClientMetrics;

    CHK(ppSampleConfiguration != NULL, STATUS_NULL_ARG);
    pSampleConfiguration = *ppSampleConfiguration;

    CHK(pSampleConfiguration != NULL, retStatus);

    // Kick of the termination sequence
    ATOMIC_STORE_BOOL(&pSampleConfiguration->appTerminateFlag, TRUE);

    if (pSampleConfiguration->appMediaCtx.mediaSenderTid != INVALID_TID_VALUE) {
        THREAD_JOIN(pSampleConfiguration->appMediaCtx.mediaSenderTid, NULL);
    }

    CHK_LOG_ERR(signalingClientGetMetrics(pSampleConfiguration->appSignalingCtx.signalingClientHandle, &signalingClientMetrics));
    CHK_LOG_ERR(logSignalingClientStats(&signalingClientMetrics));
    CHK_LOG_ERR(freeSignalingClient(&pSampleConfiguration->appSignalingCtx.signalingClientHandle));

    if (IS_VALID_TIMER_QUEUE_HANDLE(pSampleConfiguration->timerQueueHandle)) {
        cancelTimerTasks(pSampleConfiguration);
        timerQueueFree(&pSampleConfiguration->timerQueueHandle);
    }

    if (pSampleConfiguration->pPendingSignalingMessageForRemoteClient != NULL) {
        // Iterate and free all the pending queues
        stackQueueGetIterator(pSampleConfiguration->pPendingSignalingMessageForRemoteClient, &iterator);
        while (IS_VALID_ITERATOR(iterator)) {
            stackQueueIteratorGetItem(iterator, &data);
            stackQueueIteratorNext(&iterator);
            freeMessageQueue((PPendingMessageQueue) data);
        }

        stackQueueClear(pSampleConfiguration->pPendingSignalingMessageForRemoteClient, FALSE);
        stackQueueFree(pSampleConfiguration->pPendingSignalingMessageForRemoteClient);
        pSampleConfiguration->pPendingSignalingMessageForRemoteClient = NULL;
    }

    hashTableClear(pSampleConfiguration->pRtcPeerConnectionForRemoteClient);
    hashTableFree(pSampleConfiguration->pRtcPeerConnectionForRemoteClient);

    if (IS_VALID_MUTEX_VALUE(pSampleConfiguration->sampleConfigurationObjLock)) {
        MUTEX_LOCK(pSampleConfiguration->sampleConfigurationObjLock);
        locked = TRUE;
    }

    for (i = 0; i < pSampleConfiguration->streamingSessionCount; ++i) {
        retStatus = gatherIceServerStats(pSampleConfiguration->sampleStreamingSessionList[i]);
        if (STATUS_FAILED(retStatus)) {
            DLOGW("Failed to ICE Server Stats for streaming session %d: %08x", i, retStatus);
        }
        freeSampleStreamingSession(&pSampleConfiguration->sampleStreamingSessionList[i]);
    }
    if (locked) {
        MUTEX_UNLOCK(pSampleConfiguration->sampleConfigurationObjLock);
    }
    deinitKvsWebRtc();

    SAFE_MEMFREE(pSampleConfiguration->appMediaCtx.pVideoFrameBuffer);
    SAFE_MEMFREE(pSampleConfiguration->appMediaCtx.pAudioFrameBuffer);

    if (IS_VALID_CVAR_VALUE(pSampleConfiguration->cvar) && IS_VALID_MUTEX_VALUE(pSampleConfiguration->sampleConfigurationObjLock)) {
        CVAR_BROADCAST(pSampleConfiguration->cvar);
        MUTEX_LOCK(pSampleConfiguration->sampleConfigurationObjLock);
        MUTEX_UNLOCK(pSampleConfiguration->sampleConfigurationObjLock);
    }

    if (IS_VALID_MUTEX_VALUE(pSampleConfiguration->sampleConfigurationObjLock)) {
        MUTEX_FREE(pSampleConfiguration->sampleConfigurationObjLock);
    }

    if (IS_VALID_MUTEX_VALUE(pSampleConfiguration->streamingSessionListReadLock)) {
        MUTEX_FREE(pSampleConfiguration->streamingSessionListReadLock);
    }

    if (IS_VALID_MUTEX_VALUE(pSampleConfiguration->appSignalingCtx.signalingSendMessageLock)) {
        MUTEX_FREE(pSampleConfiguration->appSignalingCtx.signalingSendMessageLock);
    }

    if (IS_VALID_CVAR_VALUE(pSampleConfiguration->cvar)) {
        CVAR_FREE(pSampleConfiguration->cvar);
    }

    CHK_LOG_ERR(freeDemoCredentialProvider(pSampleConfiguration));

    if (pSampleConfiguration->pregeneratedCertificates != NULL) {
        stackQueueGetIterator(pSampleConfiguration->pregeneratedCertificates, &iterator);
        while (IS_VALID_ITERATOR(iterator)) {
            stackQueueIteratorGetItem(iterator, &data);
            stackQueueIteratorNext(&iterator);
            freeRtcCertificate((PRtcCertificate) data);
        }

        CHK_LOG_ERR(stackQueueClear(pSampleConfiguration->pregeneratedCertificates, FALSE));
        CHK_LOG_ERR(stackQueueFree(pSampleConfiguration->pregeneratedCertificates));
        pSampleConfiguration->pregeneratedCertificates = NULL;
    }
    if (pSampleConfiguration->enableFileLogging) {
        freeFileLogger();
    }
    SAFE_MEMFREE(*ppSampleConfiguration);

    retStatus = RESET_INSTRUMENTED_ALLOCATORS();
    DLOGI("All SDK allocations freed? %s..0x%08x", retStatus == STATUS_SUCCESS ? "Yes" : "No", retStatus);
CleanUp:

    LEAVES();
    return retStatus;
}

STATUS sessionCleanupWait(PSampleConfiguration pSampleConfiguration)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PSampleStreamingSession pSampleStreamingSession = NULL;
    UINT32 i, clientIdHash;
    BOOL sampleConfigurationObjLockLocked = FALSE, streamingSessionListReadLockLocked = FALSE, peerConnectionFound = FALSE, sessionFreed = FALSE;
    SIGNALING_CLIENT_STATE signalingClientState;

    CHK(pSampleConfiguration != NULL, STATUS_NULL_ARG);

    while (!ATOMIC_LOAD_BOOL(&pSampleConfiguration->interrupted)) {
        // Keep the main set of operations interlocked until cvar wait which would atomically unlock
        MUTEX_LOCK(pSampleConfiguration->sampleConfigurationObjLock);
        sampleConfigurationObjLockLocked = TRUE;

        // scan and cleanup terminated streaming session
        for (i = 0; i < pSampleConfiguration->streamingSessionCount; ++i) {
            if (ATOMIC_LOAD_BOOL(&pSampleConfiguration->sampleStreamingSessionList[i]->terminateFlag)) {
                pSampleStreamingSession = pSampleConfiguration->sampleStreamingSessionList[i];

                MUTEX_LOCK(pSampleConfiguration->streamingSessionListReadLock);
                streamingSessionListReadLockLocked = TRUE;

                // swap with last element and decrement count
                pSampleConfiguration->streamingSessionCount--;
                pSampleConfiguration->sampleStreamingSessionList[i] =
                    pSampleConfiguration->sampleStreamingSessionList[pSampleConfiguration->streamingSessionCount];

                // Remove from the hash table
                clientIdHash = COMPUTE_CRC32((PBYTE) pSampleStreamingSession->peerId, (UINT32) STRLEN(pSampleStreamingSession->peerId));
                CHK_STATUS(hashTableContains(pSampleConfiguration->pRtcPeerConnectionForRemoteClient, clientIdHash, &peerConnectionFound));
                if (peerConnectionFound) {
                    CHK_STATUS(hashTableRemove(pSampleConfiguration->pRtcPeerConnectionForRemoteClient, clientIdHash));
                }

                MUTEX_UNLOCK(pSampleConfiguration->streamingSessionListReadLock);
                streamingSessionListReadLockLocked = FALSE;

                CHK_STATUS(freeSampleStreamingSession(&pSampleStreamingSession));
                sessionFreed = TRUE;
            }
        }

        if (sessionFreed && pSampleConfiguration->appSignalingCtx.channelInfo.useMediaStorage && !ATOMIC_LOAD_BOOL(&pSampleConfiguration->appSignalingCtx.recreateSignalingClient)) {
            // In the WebRTC Media Storage Ingestion Case the backend will terminate the session after
            // 1 hour.  The SDK needs to make a new JoinSession Call in order to receive a new
            // offer from the backend.  We will create a new sample streaming session upon receipt of the
            // offer.  The signalingClientConnectSync call will result in a JoinSession API call being made.
            CHK_STATUS(signalingClientDisconnectSync(pSampleConfiguration->appSignalingCtx.signalingClientHandle));
            CHK_STATUS(signalingClientFetchSync(pSampleConfiguration->appSignalingCtx.signalingClientHandle));
            CHK_STATUS(signalingClientConnectSync(pSampleConfiguration->appSignalingCtx.signalingClientHandle));
            sessionFreed = FALSE;
        }

        // Check if we need to re-create the signaling client on-the-fly
        if (ATOMIC_LOAD_BOOL(&pSampleConfiguration->appSignalingCtx.recreateSignalingClient)) {
            retStatus = signalingClientFetchSync(pSampleConfiguration->appSignalingCtx.signalingClientHandle);
            if (STATUS_SUCCEEDED(retStatus)) {
                // Re-set the variable again
                ATOMIC_STORE_BOOL(&pSampleConfiguration->appSignalingCtx.recreateSignalingClient, FALSE);
            } else if (signalingCallFailed(retStatus)) {
                printf("[KVS Common] recreating Signaling Client\n");
                freeSignalingClient(&pSampleConfiguration->appSignalingCtx.signalingClientHandle);
                createSignalingClientSync(&pSampleConfiguration->appSignalingCtx.clientInfo, &pSampleConfiguration->appSignalingCtx.channelInfo,
                                          &pSampleConfiguration->appSignalingCtx.signalingClientCallbacks, pSampleConfiguration->pCredentialProvider,
                                          &pSampleConfiguration->appSignalingCtx.signalingClientHandle);
            }
        }

        // Check the signaling client state and connect if needed
        if (IS_VALID_SIGNALING_CLIENT_HANDLE(pSampleConfiguration->appSignalingCtx.signalingClientHandle)) {
            CHK_STATUS(signalingClientGetCurrentState(pSampleConfiguration->appSignalingCtx.signalingClientHandle, &signalingClientState));
            if (signalingClientState == SIGNALING_CLIENT_STATE_READY) {
                UNUSED_PARAM(signalingClientConnectSync(pSampleConfiguration->appSignalingCtx.signalingClientHandle));
            }
        }

        // Check if any lingering pending message queues
        CHK_STATUS(removeExpiredMessageQueues(pSampleConfiguration->pPendingSignalingMessageForRemoteClient));

        // periodically wake up and clean up terminated streaming session
        CVAR_WAIT(pSampleConfiguration->cvar, pSampleConfiguration->sampleConfigurationObjLock, SAMPLE_SESSION_CLEANUP_WAIT_PERIOD);
        MUTEX_UNLOCK(pSampleConfiguration->sampleConfigurationObjLock);
        sampleConfigurationObjLockLocked = FALSE;
    }

CleanUp:

    CHK_LOG_ERR(retStatus);

    if (sampleConfigurationObjLockLocked) {
        MUTEX_UNLOCK(pSampleConfiguration->sampleConfigurationObjLock);
    }

    if (streamingSessionListReadLockLocked) {
        MUTEX_UNLOCK(pSampleConfiguration->streamingSessionListReadLock);
    }

    LEAVES();
    return retStatus;
}

STATUS submitPendingIceCandidate(PPendingMessageQueue pPendingMessageQueue, PSampleStreamingSession pSampleStreamingSession)
{
    STATUS retStatus = STATUS_SUCCESS;
    BOOL noPendingSignalingMessageForClient = FALSE;
    PReceivedSignalingMessage pReceivedSignalingMessage = NULL;
    UINT64 hashValue;

    CHK(pPendingMessageQueue != NULL && pPendingMessageQueue->messageQueue != NULL && pSampleStreamingSession != NULL, STATUS_NULL_ARG);

    do {
        CHK_STATUS(stackQueueIsEmpty(pPendingMessageQueue->messageQueue, &noPendingSignalingMessageForClient));
        if (!noPendingSignalingMessageForClient) {
            hashValue = 0;
            CHK_STATUS(stackQueueDequeue(pPendingMessageQueue->messageQueue, &hashValue));
            pReceivedSignalingMessage = (PReceivedSignalingMessage) hashValue;
            CHK(pReceivedSignalingMessage != NULL, STATUS_INTERNAL_ERROR);
            if (pReceivedSignalingMessage->signalingMessage.messageType == SIGNALING_MESSAGE_TYPE_ICE_CANDIDATE) {
                CHK_STATUS(handleRemoteCandidate(pSampleStreamingSession, &pReceivedSignalingMessage->signalingMessage));
            }
            SAFE_MEMFREE(pReceivedSignalingMessage);
        }
    } while (!noPendingSignalingMessageForClient);

    CHK_STATUS(freeMessageQueue(pPendingMessageQueue));

CleanUp:

    SAFE_MEMFREE(pReceivedSignalingMessage);
    CHK_LOG_ERR(retStatus);
    return retStatus;
}

STATUS signalingMessageReceived(UINT64 customData, PReceivedSignalingMessage pReceivedSignalingMessage)
{
    STATUS retStatus = STATUS_SUCCESS;
    PSampleConfiguration pSampleConfiguration = (PSampleConfiguration) customData;
    BOOL peerConnectionFound = FALSE, locked = FALSE, freeStreamingSession = FALSE;
    UINT32 clientIdHash;
    UINT64 hashValue = 0;
    PPendingMessageQueue pPendingMessageQueue = NULL;
    PSampleStreamingSession pSampleStreamingSession = NULL;
    PReceivedSignalingMessage pReceivedSignalingMessageCopy = NULL;

    CHK(pSampleConfiguration != NULL, STATUS_NULL_ARG);

    MUTEX_LOCK(pSampleConfiguration->sampleConfigurationObjLock);
    locked = TRUE;

    clientIdHash = COMPUTE_CRC32((PBYTE) pReceivedSignalingMessage->signalingMessage.peerClientId,
                                 (UINT32) STRLEN(pReceivedSignalingMessage->signalingMessage.peerClientId));
    CHK_STATUS(hashTableContains(pSampleConfiguration->pRtcPeerConnectionForRemoteClient, clientIdHash, &peerConnectionFound));
    if (peerConnectionFound) {
        CHK_STATUS(hashTableGet(pSampleConfiguration->pRtcPeerConnectionForRemoteClient, clientIdHash, &hashValue));
        pSampleStreamingSession = (PSampleStreamingSession) hashValue;
    }

    switch (pReceivedSignalingMessage->signalingMessage.messageType) {
        case SIGNALING_MESSAGE_TYPE_OFFER:
            // Check if we already have an ongoing master session with the same peer
            CHK_ERR(!peerConnectionFound, STATUS_INVALID_OPERATION, "Peer connection %s is in progress",
                    pReceivedSignalingMessage->signalingMessage.peerClientId);

            /*
             * Create new streaming session for each offer, then insert the client id and streaming session into
             * pRtcPeerConnectionForRemoteClient for subsequent ice candidate messages. Lastly check if there is
             * any ice candidate messages queued in pPendingSignalingMessageForRemoteClient. If so then submit
             * all of them.
             */

            if (pSampleConfiguration->streamingSessionCount == ARRAY_SIZE(pSampleConfiguration->sampleStreamingSessionList)) {
                DLOGW("Max simultaneous streaming session count reached.");

                // Need to remove the pending queue if any.
                // This is a simple optimization as the session cleanup will
                // handle the cleanup of pending message queue after a while
                CHK_STATUS(getPendingMessageQueueForHash(pSampleConfiguration->pPendingSignalingMessageForRemoteClient, clientIdHash, TRUE,
                                                         &pPendingMessageQueue));

                CHK(FALSE, retStatus);
            }
            CHK_STATUS(createStreamingSession(pSampleConfiguration, pReceivedSignalingMessage->signalingMessage.peerClientId, TRUE,
                                              &pSampleStreamingSession));
            freeStreamingSession = TRUE;
            CHK_STATUS(handleOffer(pSampleConfiguration, pSampleStreamingSession, &pReceivedSignalingMessage->signalingMessage));
            CHK_STATUS(hashTablePut(pSampleConfiguration->pRtcPeerConnectionForRemoteClient, clientIdHash, (UINT64) pSampleStreamingSession));

            // If there are any ice candidate messages in the queue for this client id, submit them now.
            CHK_STATUS(getPendingMessageQueueForHash(pSampleConfiguration->pPendingSignalingMessageForRemoteClient, clientIdHash, TRUE,
                                                     &pPendingMessageQueue));
            if (pPendingMessageQueue != NULL) {
                CHK_STATUS(submitPendingIceCandidate(pPendingMessageQueue, pSampleStreamingSession));

                // NULL the pointer to avoid it being freed in the cleanup
                pPendingMessageQueue = NULL;
            }

            MUTEX_LOCK(pSampleConfiguration->streamingSessionListReadLock);
            pSampleConfiguration->sampleStreamingSessionList[pSampleConfiguration->streamingSessionCount++] = pSampleStreamingSession;
            MUTEX_UNLOCK(pSampleConfiguration->streamingSessionListReadLock);
            freeStreamingSession = FALSE;

            break;

        case SIGNALING_MESSAGE_TYPE_ANSWER:
            /*
             * for viewer, pSampleStreamingSession should've already been created. insert the client id and
             * streaming session into pRtcPeerConnectionForRemoteClient for subsequent ice candidate messages.
             * Lastly check if there is any ice candidate messages queued in pPendingSignalingMessageForRemoteClient.
             * If so then submit all of them.
             */
            pSampleStreamingSession = pSampleConfiguration->sampleStreamingSessionList[0];
            CHK_STATUS(handleAnswer(pSampleConfiguration, pSampleStreamingSession, &pReceivedSignalingMessage->signalingMessage));
            CHK_STATUS(hashTablePut(pSampleConfiguration->pRtcPeerConnectionForRemoteClient, clientIdHash, (UINT64) pSampleStreamingSession));

            // If there are any ice candidate messages in the queue for this client id, submit them now.
            CHK_STATUS(getPendingMessageQueueForHash(pSampleConfiguration->pPendingSignalingMessageForRemoteClient, clientIdHash, TRUE,
                                                     &pPendingMessageQueue));
            if (pPendingMessageQueue != NULL) {
                CHK_STATUS(submitPendingIceCandidate(pPendingMessageQueue, pSampleStreamingSession));

                // NULL the pointer to avoid it being freed in the cleanup
                pPendingMessageQueue = NULL;
            }

            CHK_STATUS(signalingClientGetMetrics(pSampleConfiguration->appSignalingCtx.signalingClientHandle, &pSampleConfiguration->appSignalingCtx.signalingClientMetrics));
            DLOGP("[Signaling offer sent to answer received time] %" PRIu64 " ms",
                  pSampleConfiguration->appSignalingCtx.signalingClientMetrics.signalingClientStats.offerToAnswerTime);
            break;

        case SIGNALING_MESSAGE_TYPE_ICE_CANDIDATE:
            /*
             * if peer connection hasn't been created, create an queue to store the ice candidate message. Otherwise
             * submit the signaling message into the corresponding streaming session.
             */
            if (!peerConnectionFound) {
                CHK_STATUS(getPendingMessageQueueForHash(pSampleConfiguration->pPendingSignalingMessageForRemoteClient, clientIdHash, FALSE,
                                                         &pPendingMessageQueue));
                if (pPendingMessageQueue == NULL) {
                    CHK_STATUS(createMessageQueue(clientIdHash, &pPendingMessageQueue));
                    CHK_STATUS(stackQueueEnqueue(pSampleConfiguration->pPendingSignalingMessageForRemoteClient, (UINT64) pPendingMessageQueue));
                }

                pReceivedSignalingMessageCopy = (PReceivedSignalingMessage) MEMCALLOC(1, SIZEOF(ReceivedSignalingMessage));

                *pReceivedSignalingMessageCopy = *pReceivedSignalingMessage;

                CHK_STATUS(stackQueueEnqueue(pPendingMessageQueue->messageQueue, (UINT64) pReceivedSignalingMessageCopy));

                // NULL the pointers to not free any longer
                pPendingMessageQueue = NULL;
                pReceivedSignalingMessageCopy = NULL;
            } else {
                CHK_STATUS(handleRemoteCandidate(pSampleStreamingSession, &pReceivedSignalingMessage->signalingMessage));
            }
            break;

        default:
            DLOGD("Unhandled signaling message type %u", pReceivedSignalingMessage->signalingMessage.messageType);
            break;
    }

    MUTEX_UNLOCK(pSampleConfiguration->sampleConfigurationObjLock);
    locked = FALSE;

CleanUp:

    SAFE_MEMFREE(pReceivedSignalingMessageCopy);
    if (pPendingMessageQueue != NULL) {
        freeMessageQueue(pPendingMessageQueue);
    }

    if (freeStreamingSession && pSampleStreamingSession != NULL) {
        freeSampleStreamingSession(&pSampleStreamingSession);
    }

    if (locked) {
        MUTEX_UNLOCK(pSampleConfiguration->sampleConfigurationObjLock);
    }

    CHK_LOG_ERR(retStatus);
    return retStatus;
}

STATUS createMessageQueue(UINT64 hashValue, PPendingMessageQueue* ppPendingMessageQueue)
{
    STATUS retStatus = STATUS_SUCCESS;
    PPendingMessageQueue pPendingMessageQueue = NULL;

    CHK(ppPendingMessageQueue != NULL, STATUS_NULL_ARG);

    CHK(NULL != (pPendingMessageQueue = (PPendingMessageQueue) MEMCALLOC(1, SIZEOF(PendingMessageQueue))), STATUS_NOT_ENOUGH_MEMORY);
    pPendingMessageQueue->hashValue = hashValue;
    pPendingMessageQueue->createTime = GETTIME();
    CHK_STATUS(stackQueueCreate(&pPendingMessageQueue->messageQueue));

CleanUp:

    if (STATUS_FAILED(retStatus) && pPendingMessageQueue != NULL) {
        freeMessageQueue(pPendingMessageQueue);
        pPendingMessageQueue = NULL;
    }

    if (ppPendingMessageQueue != NULL) {
        *ppPendingMessageQueue = pPendingMessageQueue;
    }

    return retStatus;
}

STATUS freeMessageQueue(PPendingMessageQueue pPendingMessageQueue)
{
    STATUS retStatus = STATUS_SUCCESS;

    // free is idempotent
    CHK(pPendingMessageQueue != NULL, retStatus);

    if (pPendingMessageQueue->messageQueue != NULL) {
        stackQueueClear(pPendingMessageQueue->messageQueue, TRUE);
        stackQueueFree(pPendingMessageQueue->messageQueue);
    }

    MEMFREE(pPendingMessageQueue);

CleanUp:
    return retStatus;
}

STATUS getPendingMessageQueueForHash(PStackQueue pPendingQueue, UINT64 clientHash, BOOL remove, PPendingMessageQueue* ppPendingMessageQueue)
{
    STATUS retStatus = STATUS_SUCCESS;
    PPendingMessageQueue pPendingMessageQueue = NULL;
    StackQueueIterator iterator;
    BOOL iterate = TRUE;
    UINT64 data;

    CHK(pPendingQueue != NULL && ppPendingMessageQueue != NULL, STATUS_NULL_ARG);

    CHK_STATUS(stackQueueGetIterator(pPendingQueue, &iterator));
    while (iterate && IS_VALID_ITERATOR(iterator)) {
        CHK_STATUS(stackQueueIteratorGetItem(iterator, &data));
        CHK_STATUS(stackQueueIteratorNext(&iterator));

        pPendingMessageQueue = (PPendingMessageQueue) data;

        if (clientHash == pPendingMessageQueue->hashValue) {
            *ppPendingMessageQueue = pPendingMessageQueue;
            iterate = FALSE;

            // Check if the item needs to be removed
            if (remove) {
                // This is OK to do as we are terminating the iterator anyway
                CHK_STATUS(stackQueueRemoveItem(pPendingQueue, data));
            }
        }
    }

CleanUp:

    return retStatus;
}

STATUS removeExpiredMessageQueues(PStackQueue pPendingQueue)
{
    STATUS retStatus = STATUS_SUCCESS;
    PPendingMessageQueue pPendingMessageQueue = NULL;
    UINT32 i, count;
    UINT64 data, curTime;

    CHK(pPendingQueue != NULL, STATUS_NULL_ARG);

    curTime = GETTIME();
    CHK_STATUS(stackQueueGetCount(pPendingQueue, &count));

    // Dequeue and enqueue in order to not break the iterator while removing an item
    for (i = 0; i < count; i++) {
        CHK_STATUS(stackQueueDequeue(pPendingQueue, &data));

        // Check for expiry
        pPendingMessageQueue = (PPendingMessageQueue) data;
        if (pPendingMessageQueue->createTime + SAMPLE_PENDING_MESSAGE_CLEANUP_DURATION < curTime) {
            // Message queue has expired and needs to be freed
            CHK_STATUS(freeMessageQueue(pPendingMessageQueue));
        } else {
            // Enqueue back again as it's still valued
            CHK_STATUS(stackQueueEnqueue(pPendingQueue, data));
        }
    }

CleanUp:

    return retStatus;
}

#ifdef ENABLE_DATA_CHANNEL
VOID onDataChannelMessage(UINT64 customData, PRtcDataChannel pDataChannel, BOOL isBinary, PBYTE pMessage, UINT32 pMessageLen)
{
    STATUS retStatus = STATUS_SUCCESS;
    UINT32 i, strLen, tokenCount;
    CHAR pMessageSend[MAX_DATA_CHANNEL_METRICS_MESSAGE_SIZE], errorMessage[200];
    PCHAR json;
    PSampleStreamingSession pSampleStreamingSession = (PSampleStreamingSession) customData;
    PSampleConfiguration pSampleConfiguration;
    DataChannelMessage dataChannelMessage;
    jsmn_parser parser;
    jsmntok_t tokens[MAX_JSON_TOKEN_COUNT];

    CHK(pMessage != NULL && pDataChannel != NULL, STATUS_NULL_ARG);

    if (pSampleStreamingSession == NULL) {
        STRCPY(errorMessage, "Could not generate stats since the streaming session is NULL");
        retStatus = dataChannelSend(pDataChannel, FALSE, (PBYTE) errorMessage, STRLEN(errorMessage));
        DLOGE("%s", errorMessage);
        goto CleanUp;
    }

    pSampleConfiguration = pSampleStreamingSession->pSampleConfiguration;
    if (pSampleConfiguration == NULL) {
        STRCPY(errorMessage, "Could not generate stats since the sample configuration is NULL");
        retStatus = dataChannelSend(pDataChannel, FALSE, (PBYTE) errorMessage, STRLEN(errorMessage));
        DLOGE("%s", errorMessage);
        goto CleanUp;
    }

    if (pSampleConfiguration->appConfigCtx.enableSendingMetricsToViewerViaDc) {
        jsmn_init(&parser);
        json = (PCHAR) pMessage;
        tokenCount = jsmn_parse(&parser, json, STRLEN(json), tokens, SIZEOF(tokens) / SIZEOF(jsmntok_t));

        MEMSET(dataChannelMessage.content, '\0', SIZEOF(dataChannelMessage.content));
        MEMSET(dataChannelMessage.firstMessageFromViewerTs, '\0', SIZEOF(dataChannelMessage.firstMessageFromViewerTs));
        MEMSET(dataChannelMessage.firstMessageFromMasterTs, '\0', SIZEOF(dataChannelMessage.firstMessageFromMasterTs));
        MEMSET(dataChannelMessage.secondMessageFromViewerTs, '\0', SIZEOF(dataChannelMessage.secondMessageFromViewerTs));
        MEMSET(dataChannelMessage.secondMessageFromMasterTs, '\0', SIZEOF(dataChannelMessage.secondMessageFromMasterTs));
        MEMSET(dataChannelMessage.lastMessageFromViewerTs, '\0', SIZEOF(dataChannelMessage.lastMessageFromViewerTs));

        if (tokenCount > 1) {
            if (tokens[0].type != JSMN_OBJECT) {
                STRCPY(errorMessage, "Invalid JSON received, please send a valid json as the SDK is operating in datachannel-benchmarking mode");
                retStatus = dataChannelSend(pDataChannel, FALSE, (PBYTE) errorMessage, STRLEN(errorMessage));
                DLOGE("%s", errorMessage);
                retStatus = STATUS_INVALID_API_CALL_RETURN_JSON;
                goto CleanUp;
            }
            DLOGI("DataChannel json message: %.*s\n", pMessageLen, pMessage);

            for (i = 1; i < tokenCount; i++) {
                if (compareJsonString(json, &tokens[i], JSMN_STRING, (PCHAR) "content")) {
                    strLen = (UINT32) (tokens[i + 1].end - tokens[i + 1].start);
                    if (strLen != 0) {
                        STRNCPY(dataChannelMessage.content, json + tokens[i + 1].start, tokens[i + 1].end - tokens[i + 1].start);
                    }
                } else if (compareJsonString(json, &tokens[i], JSMN_STRING, (PCHAR) "firstMessageFromViewerTs")) {
                    strLen = (UINT32) (tokens[i + 1].end - tokens[i + 1].start);
                    // parse and retain this message from the viewer to send it back again
                    if (strLen != 0) {
                        // since the length is not zero, we have already attached this timestamp to structure in the last iteration
                        STRNCPY(dataChannelMessage.firstMessageFromViewerTs, json + tokens[i + 1].start, tokens[i + 1].end - tokens[i + 1].start);
                    }
                } else if (compareJsonString(json, &tokens[i], JSMN_STRING, (PCHAR) "firstMessageFromMasterTs")) {
                    strLen = (UINT32) (tokens[i + 1].end - tokens[i + 1].start);
                    if (strLen != 0) {
                        // since the length is not zero, we have already attached this timestamp to structure in the last iteration
                        STRNCPY(dataChannelMessage.firstMessageFromMasterTs, json + tokens[i + 1].start, tokens[i + 1].end - tokens[i + 1].start);
                    } else {
                        // if this timestamp was not assigned during the previous message session, add it now
                        SNPRINTF(dataChannelMessage.firstMessageFromMasterTs, 20, "%llu", GETTIME() / 10000);
                        break;
                    }
                } else if (compareJsonString(json, &tokens[i], JSMN_STRING, (PCHAR) "secondMessageFromViewerTs")) {
                    strLen = (UINT32) (tokens[i + 1].end - tokens[i + 1].start);
                    // parse and retain this message from the viewer to send it back again
                    if (strLen != 0) {
                        STRNCPY(dataChannelMessage.secondMessageFromViewerTs, json + tokens[i + 1].start, tokens[i + 1].end - tokens[i + 1].start);
                    }
                } else if (compareJsonString(json, &tokens[i], JSMN_STRING, (PCHAR) "secondMessageFromMasterTs")) {
                    strLen = (UINT32) (tokens[i + 1].end - tokens[i + 1].start);
                    if (strLen != 0) {
                        // since the length is not zero, we have already attached this timestamp to structure in the last iteration
                        STRNCPY(dataChannelMessage.secondMessageFromMasterTs, json + tokens[i + 1].start, tokens[i + 1].end - tokens[i + 1].start);
                    } else {
                        // if this timestamp was not assigned during the previous message session, add it now
                        SNPRINTF(dataChannelMessage.secondMessageFromMasterTs, 20, "%llu", GETTIME() / 10000);
                        break;
                    }
                } else if (compareJsonString(json, &tokens[i], JSMN_STRING, (PCHAR) "lastMessageFromViewerTs")) {
                    strLen = (UINT32) (tokens[i + 1].end - tokens[i + 1].start);
                    if (strLen != 0) {
                        STRNCPY(dataChannelMessage.lastMessageFromViewerTs, json + tokens[i + 1].start, tokens[i + 1].end - tokens[i + 1].start);
                    }
                }
            }

            if (STRLEN(dataChannelMessage.lastMessageFromViewerTs) == 0) {
                // continue sending the data_channel_metrics_message with new timestamps until we receive the lastMessageFromViewerTs from the viewer
                SNPRINTF(pMessageSend, MAX_DATA_CHANNEL_METRICS_MESSAGE_SIZE, DATA_CHANNEL_MESSAGE_TEMPLATE, MASTER_DATA_CHANNEL_MESSAGE,
                         dataChannelMessage.firstMessageFromViewerTs, dataChannelMessage.firstMessageFromMasterTs,
                         dataChannelMessage.secondMessageFromViewerTs, dataChannelMessage.secondMessageFromMasterTs,
                         dataChannelMessage.lastMessageFromViewerTs);
                DLOGI("Master's response: %s", pMessageSend);

                retStatus = dataChannelSend(pDataChannel, FALSE, (PBYTE) pMessageSend, STRLEN(pMessageSend));
            } else {
                // now that we've received the last message, send across the signaling, peerConnection, ice metrics
                SNPRINTF(pSampleStreamingSession->pSignalingClientMetricsMessage, MAX_SIGNALING_CLIENT_METRICS_MESSAGE_SIZE,
                         SIGNALING_CLIENT_METRICS_JSON_TEMPLATE, pSampleConfiguration->appSignalingCtx.signalingClientMetrics.signalingStartTime,
                         pSampleConfiguration->appSignalingCtx.signalingClientMetrics.signalingEndTime,
                         pSampleConfiguration->appSignalingCtx.signalingClientMetrics.signalingClientStats.offerReceivedTime,
                         pSampleConfiguration->appSignalingCtx.signalingClientMetrics.signalingClientStats.answerTime,
                         pSampleConfiguration->appSignalingCtx.signalingClientMetrics.signalingClientStats.describeChannelStartTime,
                         pSampleConfiguration->appSignalingCtx.signalingClientMetrics.signalingClientStats.describeChannelEndTime,
                         pSampleConfiguration->appSignalingCtx.signalingClientMetrics.signalingClientStats.getSignalingChannelEndpointStartTime,
                         pSampleConfiguration->appSignalingCtx.signalingClientMetrics.signalingClientStats.getSignalingChannelEndpointEndTime,
                         pSampleConfiguration->appSignalingCtx.signalingClientMetrics.signalingClientStats.getIceServerConfigStartTime,
                         pSampleConfiguration->appSignalingCtx.signalingClientMetrics.signalingClientStats.getIceServerConfigEndTime,
                         pSampleConfiguration->appSignalingCtx.signalingClientMetrics.signalingClientStats.getTokenStartTime,
                         pSampleConfiguration->appSignalingCtx.signalingClientMetrics.signalingClientStats.getTokenEndTime,
                         pSampleConfiguration->appSignalingCtx.signalingClientMetrics.signalingClientStats.createChannelStartTime,
                         pSampleConfiguration->appSignalingCtx.signalingClientMetrics.signalingClientStats.createChannelEndTime,
                         pSampleConfiguration->appSignalingCtx.signalingClientMetrics.signalingClientStats.connectStartTime,
                         pSampleConfiguration->appSignalingCtx.signalingClientMetrics.signalingClientStats.connectEndTime);
                DLOGI("Sending signaling metrics to the viewer: %s", pSampleStreamingSession->pSignalingClientMetricsMessage);

                CHK_STATUS(peerConnectionGetMetrics(pSampleStreamingSession->pPeerConnection, &pSampleStreamingSession->peerConnectionMetrics));
                SNPRINTF(pSampleStreamingSession->pPeerConnectionMetricsMessage, MAX_PEER_CONNECTION_METRICS_MESSAGE_SIZE,
                         PEER_CONNECTION_METRICS_JSON_TEMPLATE,
                         pSampleStreamingSession->peerConnectionMetrics.peerConnectionStats.peerConnectionStartTime,
                         pSampleStreamingSession->peerConnectionMetrics.peerConnectionStats.peerConnectionConnectedTime);
                DLOGI("Sending peer-connection metrics to the viewer: %s", pSampleStreamingSession->pPeerConnectionMetricsMessage);

                CHK_STATUS(iceAgentGetMetrics(pSampleStreamingSession->pPeerConnection, &pSampleStreamingSession->iceMetrics));
                SNPRINTF(pSampleStreamingSession->pIceAgentMetricsMessage, MAX_ICE_AGENT_METRICS_MESSAGE_SIZE, ICE_AGENT_METRICS_JSON_TEMPLATE,
                         pSampleStreamingSession->iceMetrics.kvsIceAgentStats.candidateGatheringStartTime,
                         pSampleStreamingSession->iceMetrics.kvsIceAgentStats.candidateGatheringEndTime);
                DLOGI("Sending ice-agent metrics to the viewer: %s", pSampleStreamingSession->pIceAgentMetricsMessage);

                retStatus = dataChannelSend(pDataChannel, FALSE, (PBYTE) pSampleStreamingSession->pSignalingClientMetricsMessage,
                                            STRLEN(pSampleStreamingSession->pSignalingClientMetricsMessage));
                retStatus = dataChannelSend(pDataChannel, FALSE, (PBYTE) pSampleStreamingSession->pPeerConnectionMetricsMessage,
                                            STRLEN(pSampleStreamingSession->pPeerConnectionMetricsMessage));
                retStatus = dataChannelSend(pDataChannel, FALSE, (PBYTE) pSampleStreamingSession->pIceAgentMetricsMessage,
                                            STRLEN(pSampleStreamingSession->pIceAgentMetricsMessage));
            }
        } else {
            DLOGI("DataChannel string message: %.*s\n", pMessageLen, pMessage);
            STRCPY(errorMessage, "Send a json message for benchmarking as the C SDK is operating in benchmarking mode");
            retStatus = dataChannelSend(pDataChannel, FALSE, (PBYTE) errorMessage, STRLEN(errorMessage));
        }
    } else {
        if (isBinary) {
            DLOGI("DataChannel Binary Message");
        } else {
            DLOGI("DataChannel String Message: %.*s\n", pMessageLen, pMessage);
        }
        // Send a response to the message sent by the viewer
        retStatus = dataChannelSend(pDataChannel, FALSE, (PBYTE) MASTER_DATA_CHANNEL_MESSAGE, STRLEN(MASTER_DATA_CHANNEL_MESSAGE));
    }
    if (retStatus != STATUS_SUCCESS) {
        DLOGI("[KVS Master] dataChannelSend(): operation returned status code: 0x%08x \n", retStatus);
    }

CleanUp:
    CHK_LOG_ERR(retStatus);
}

VOID onDataChannel(UINT64 customData, PRtcDataChannel pRtcDataChannel)
{
    DLOGI("New DataChannel has been opened %s \n", pRtcDataChannel->name);
    dataChannelOnMessage(pRtcDataChannel, customData, onDataChannelMessage);
}
#endif
