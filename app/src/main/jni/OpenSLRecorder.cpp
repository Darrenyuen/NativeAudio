//
// Created by darrenyuan on 2022/11/6.
//
// for native audio
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

#include <cassert>
#include <android/log.h>
#include <pthread.h>
#include <fstream>
#include <vector>

#include "com_darrenyuan_nativefeedback_OpenSLRecorder.h"

#define LOG_TAG "NativeOpenSLRecorder"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

// engine interfaces
static SLObjectItf engineObject = NULL;
static SLEngineItf engineEngine;

// a mutext to guard against re-entrance to record & playback
// as well as make recording and playing back to be mutually exclusive
// this is to avoid crash at situations like:
//    recording is in session [not finished]
//    user presses record button and another recording coming in
// The action: when recording/playing back is not finished, ignore the new
// request
static pthread_mutex_t audioEngineLock = PTHREAD_MUTEX_INITIALIZER;

// output mix interfaces
static SLObjectItf outputMixObject = NULL;
static SLEnvironmentalReverbItf outputMixEnvironmentalReverb = NULL;

// aux effect on the output mix, used by the buffer queue player
static const SLEnvironmentalReverbSettings reverbSettings =
        SL_I3DL2_ENVIRONMENT_PRESET_STONECORRIDOR;

// recorder interfaces
static SLObjectItf recorderObject = NULL;
static SLRecordItf recorderRecord;
static SLAndroidSimpleBufferQueueItf recorderBufferQueue;

// buffer queue player interfaces
static SLObjectItf bqPlayerObject = NULL;
static SLPlayItf bqPlayerPlay;
static SLAndroidSimpleBufferQueueItf bqPlayerBufferQueue;
static SLEffectSendItf bqPlayerEffectSend;
static SLMuteSoloItf bqPlayerMuteSolo;
static SLVolumeItf bqPlayerVolume;
static SLmilliHertz bqPlayerSampleRate = 0;
static jint bqPlayerBufSize = 0;
static short* resampleBuf = NULL;

// pointer and size of the next player buffer to enqueue
const int PLAYER_BUFFER_COUNT = 50;
const int BUFFER_SIZE = 1024;
static unsigned char *playerBuffers = NULL;
std::fstream inputFs;

// 5 seconds of recorded audio at 44.1 kHz mono, 16-bit signed little endian
#define RECORDER_FRAMES (44100 * 5)
static short recorderBuffer[RECORDER_FRAMES];
static unsigned recorderSize = 0;
static const char* pcmDstPathPtr;

#ifdef __cplusplus
extern "C" {
#endif
void createEngine()
{
    LOGI("createEngine");
    SLresult result;

    // create engine
    result = slCreateEngine(&engineObject, 0, NULL, 0, NULL, NULL);
    assert(SL_RESULT_SUCCESS == result);
    (void) result;

    // realize the engine
    result = (*engineObject)->Realize(engineObject, SL_BOOLEAN_FALSE);
    assert(SL_RESULT_SUCCESS == result);
    (void) result;

    // get the engine interface, which is needed in order to create other objects
    result = (*engineObject)->GetInterface(engineObject, SL_IID_ENGINE, &engineEngine);
    assert(SL_RESULT_SUCCESS == result);
    (void) result;

    // create output mix, with environmental reverb specified as a non-required
    // interface
    const SLInterfaceID ids[1] = {SL_IID_ENVIRONMENTALREVERB};
    const SLboolean req[1] = {SL_BOOLEAN_FALSE};
    result = (*engineEngine)
            ->CreateOutputMix(engineEngine, &outputMixObject, 1, ids, req);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;

    // realize the output mix
    result = (*outputMixObject)->Realize(outputMixObject, SL_BOOLEAN_FALSE);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;

    // get the environmental reverb interface
    // this could fail if the environmental reverb effect is not available,
    // either because the feature is not present, excessive CPU load, or
    // the required MODIFY_AUDIO_SETTINGS permission was not requested and granted
    result = (*outputMixObject)
            ->GetInterface(outputMixObject, SL_IID_ENVIRONMENTALREVERB,
                           &outputMixEnvironmentalReverb);
    if (SL_RESULT_SUCCESS == result) {
        result = (*outputMixEnvironmentalReverb)
                ->SetEnvironmentalReverbProperties(
                        outputMixEnvironmentalReverb, &reverbSettings);
        (void)result;
    }
    // ignore unsuccessful result codes for environmental reverb, as it is
    // optional for this example
}

// this callback handler is called every time a buffer finishes recording
void bqRecorderCallback(SLAndroidSimpleBufferQueueItf bq, void* context) {
    assert(bq == recorderBufferQueue);
    assert(NULL == context);
    // for streaming recording, here we would call Enqueue to give recorder the
    // next buffer to fill but instead, this is a one-time buffer so we stop
    // recording
    SLresult result;
    result =
            (*recorderRecord)->SetRecordState(recorderRecord, SL_RECORDSTATE_STOPPED);
    if (SL_RESULT_SUCCESS == result) {
        recorderSize = RECORDER_FRAMES * sizeof(short);
        LOGI("bqRecorderCallback fill 5s's buffer,sizeof short is %d,  buffer size is %d", sizeof(short), recorderSize);
    }
    // 将录得的数据写入文件
    std::fstream outputFs;
    outputFs.open(pcmDstPathPtr, std::ios_base::out);
    outputFs.write(reinterpret_cast<const char *>(recorderBuffer), RECORDER_FRAMES * sizeof(short));
    LOGI("write pcm data done");
    assert(SL_RESULT_SUCCESS == result);
    pthread_mutex_unlock(&audioEngineLock);
}

JNIEXPORT jboolean JNICALL
Java_com_darrenyuan_nativefeedback_OpenSLEngine_createAudioRecorder(JNIEnv *env, jobject thiz)
{
    LOGI("createAudioRecorder");
    if (engineEngine == nullptr) {
        LOGI("engineEngine is null");
        createEngine();
    }

    SLresult result;

    // configure audio source
    SLDataLocator_IODevice loc_dev = {SL_DATALOCATOR_IODEVICE,
                                      SL_IODEVICE_AUDIOINPUT,
                                      SL_DEFAULTDEVICEID_AUDIOINPUT, NULL};
    SLDataSource audioSrc = {&loc_dev, NULL};

    // configure audio sink
    SLDataLocator_AndroidSimpleBufferQueue loc_bq = {
            SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 2};
    SLDataFormat_PCM format_pcm = {
            SL_DATAFORMAT_PCM,           1,
            SL_SAMPLINGRATE_44_1,          SL_PCMSAMPLEFORMAT_FIXED_16,
            SL_PCMSAMPLEFORMAT_FIXED_16, SL_SPEAKER_FRONT_CENTER,
            SL_BYTEORDER_LITTLEENDIAN};
    SLDataSink audioSnk = {&loc_bq, &format_pcm};

    // create audio recorder
    // (requires the RECORD_AUDIO permission)
    const SLInterfaceID id[1] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE};
    const SLboolean req[1] = {SL_BOOLEAN_TRUE};
    result = (*engineEngine)
            ->CreateAudioRecorder(engineEngine, &recorderObject, &audioSrc,
                                  &audioSnk, 1, id, req);
    if (SL_RESULT_SUCCESS != result) {
        return JNI_FALSE;
    }

    // realize the audio recorder
    result = (*recorderObject)->Realize(recorderObject, SL_BOOLEAN_FALSE);
    if (SL_RESULT_SUCCESS != result) {
        return JNI_FALSE;
    }

    // get the record interface
    result = (*recorderObject)
            ->GetInterface(recorderObject, SL_IID_RECORD, &recorderRecord);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;

    // get the buffer queue interface
    result = (*recorderObject)
            ->GetInterface(recorderObject, SL_IID_ANDROIDSIMPLEBUFFERQUEUE,
                           &recorderBufferQueue);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;

    // register callback on the buffer queue
    result =
            (*recorderBufferQueue)
                    ->RegisterCallback(recorderBufferQueue, bqRecorderCallback, NULL);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;

    return JNI_TRUE;
}

JNIEXPORT void JNICALL
Java_com_darrenyuan_nativefeedback_OpenSLEngine_startRecord(JNIEnv *env, jobject thiz, jstring desPath)
{
    pcmDstPathPtr = env->GetStringUTFChars(desPath, nullptr);
    LOGI("startRecord pcmDstPathPtr' value is %s", pcmDstPathPtr);

    SLresult result;

    if (pthread_mutex_trylock(&audioEngineLock)) {
        return;
    }
    // in case already recording, stop recording and clear buffer queue
    result =
            (*recorderRecord)->SetRecordState(recorderRecord, SL_RECORDSTATE_STOPPED);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;
    result = (*recorderBufferQueue)->Clear(recorderBufferQueue);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;

    // the buffer is not valid for playback yet
    recorderSize = 0;

    // enqueue an empty buffer to be filled by the recorder
    // (for streaming recording, we would enqueue at least 2 empty buffers to
    // start things off)
    result = (*recorderBufferQueue)
            ->Enqueue(recorderBufferQueue, recorderBuffer,
                      RECORDER_FRAMES * sizeof(short));
    // the most likely other result is SL_RESULT_BUFFER_INSUFFICIENT,
    // which for this code example would indicate a programming error
    assert(SL_RESULT_SUCCESS == result);
    (void)result;

    // start recording
    result = (*recorderRecord)
            ->SetRecordState(recorderRecord, SL_RECORDSTATE_RECORDING);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;
}

extern "C"
JNIEXPORT void JNICALL
Java_com_darrenyuan_nativefeedback_OpenSLEngine_stopRecord(JNIEnv *env, jobject thiz) {
    LOGI("stopRecord");
    SLresult result;
    result =
            (*recorderRecord)->SetRecordState(recorderRecord, SL_RECORDSTATE_STOPPED);
    if (SL_RESULT_SUCCESS == result) {
        LOGI("stop success");
    }
    pthread_mutex_unlock(&audioEngineLock);
}

int counter = 0;
// this callback handler is called every time a buffer finishes playing
void bqPlayerCallback(SLAndroidSimpleBufferQueueItf bq, void* context) {
    LOGI("bqPlayerCallback");
    assert(bq == bqPlayerBufferQueue);
    assert(nullptr == context);

    SLresult result = 0;
    // enqueue another buffer
//    char * buffer = getPcmBufferData();
    char inBuffer[BUFFER_SIZE * PLAYER_BUFFER_COUNT] = {0};
//    inputFs.read(tempBuffer, BUFFER_SIZE * PLAYER_BUFFER_COUNT);
//    inputFs.read(tempBuffer, BUFFER_SIZE * PLAYER_BUFFER_COUNT);
    // todo darrenyuen 每段buffer间存在杂音
    if (inputFs.read(inBuffer, sizeof(inBuffer))) {
        counter++;
        LOGI("size of buffer is %d, counter is %d", sizeof(inBuffer), counter);
        result = (*bqPlayerBufferQueue)
                ->Enqueue(bqPlayerBufferQueue, inBuffer, sizeof(inBuffer));
    } else {
        result = (*bqPlayerPlay)->SetPlayState(bqPlayerPlay, SL_PLAYSTATE_STOPPED);
    }

    // the most likely other result is SL_RESULT_BUFFER_INSUFFICIENT,
    // which for this code example would indicate a programming error
    if (SL_RESULT_SUCCESS != result) {
        pthread_mutex_unlock(&audioEngineLock);
    }
    (void)result;
    LOGI("read buffer to play done");
//    } else {
//        releaseResampleBuf();
//        pthread_mutex_unlock(&audioEngineLock);
//    }
}

void openSrcFile() {
    inputFs.open(pcmDstPathPtr, std::ios_base::in);
    inputFs.seekg(0, std::ios::end);
    long fileSize = inputFs.tellg();
    inputFs.seekg(0, std::ios::beg);
    LOGI("openSrcFile size is %ld", fileSize);
}

JNIEXPORT void JNICALL
Java_com_darrenyuan_nativefeedback_OpenSLEngine_startPlay(JNIEnv *env, jobject thiz, jstring srcFilePath) {

    pcmDstPathPtr = env->GetStringUTFChars(srcFilePath, nullptr);
    LOGI("startPlay srcFilePath' value is %s", pcmDstPathPtr);

    if (engineEngine == nullptr) {
        LOGI("start play engineEngine is null");
        createEngine();
    }

    openSrcFile();

    SLresult result;
//    if (sampleRate >= 0 && bufSize >= 0) {
//        bqPlayerSampleRate = sampleRate * 1000;
//        /*
//         * device native buffer size is another factor to minimize audio latency,
//         * not used in this sample: we only play one giant buffer here
//         */
//        bqPlayerBufSize = bufSize;
//    }

    // configure audio source
    SLDataLocator_AndroidSimpleBufferQueue loc_bufq = {
            SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, PLAYER_BUFFER_COUNT};

//    if (playerBuffers == nullptr) {
//        playerBuffers = (unsigned char *) malloc(PLAYER_BUFFER_COUNT * BUFFER_SIZE);
//        memset(playerBuffers, 0, PLAYER_BUFFER_COUNT * BUFFER_SIZE);
//    }

    SLDataFormat_PCM format_pcm = {
            SL_DATAFORMAT_PCM,           1,
            SL_SAMPLINGRATE_44_1,           SL_PCMSAMPLEFORMAT_FIXED_16,
            SL_PCMSAMPLEFORMAT_FIXED_16, SL_SPEAKER_FRONT_CENTER,
            SL_BYTEORDER_LITTLEENDIAN};

    /*
     * Enable Fast Audio when possible:  once we set the same rate to be the
     * native, fast audio path will be triggered
     */
//    if (bqPlayerSampleRate) {
//        format_pcm.samplesPerSec = bqPlayerSampleRate;  // sample rate in mili
//        // second
//    }
    SLDataSource audioSrc = {&loc_bufq, &format_pcm};

    // configure audio sink
    SLDataLocator_OutputMix loc_outmix = {SL_DATALOCATOR_OUTPUTMIX,
                                          outputMixObject};
    SLDataSink audioSnk = {&loc_outmix, NULL};

    // create output mix
//    result = (*engineEngine)->CreateOutputMix(engineEngine, &outputMixObject, 0, NULL, NULL);
//    if (SL_RESULT_SUCCESS != result) {
//        LOGI("engineEngine.CreateOutputMix failed: %d", result);
//    }

    // realize the output mix
//    result = (*outputMixObject)->Realize(outputMixObject, SL_BOOLEAN_FALSE);
//    if (SL_RESULT_SUCCESS != result) {
//        LOGI("engineEngine.Realize failed: %d", result);
//    }

    /*
     * create audio player:
     *     fast audio does not support when SL_IID_EFFECTSEND is required, skip it
     *     for fast audio case
     */
    const SLInterfaceID ids[3] = {
            SL_IID_BUFFERQUEUE, SL_IID_VOLUME, SL_IID_EFFECTSEND,
            /*SL_IID_MUTESOLO,*/};
    const SLboolean req[3] = {SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE,
            /*SL_BOOLEAN_TRUE,*/};

    result =
            (*engineEngine)
                    ->CreateAudioPlayer(engineEngine, &bqPlayerObject, &audioSrc,
                                        &audioSnk, 2, ids, req);

    assert(SL_RESULT_SUCCESS == result);
    (void)result;

    // realize the player
    result = (*bqPlayerObject)->Realize(bqPlayerObject, SL_BOOLEAN_FALSE);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;

    // get the play interface
    result = (*bqPlayerObject)
            ->GetInterface(bqPlayerObject, SL_IID_PLAY, &bqPlayerPlay);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;

    // get the buffer queue interface
    result = (*bqPlayerObject)
            ->GetInterface(bqPlayerObject, SL_IID_BUFFERQUEUE,
                           &bqPlayerBufferQueue);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;

    // register callback on the buffer queue
    result = (*bqPlayerBufferQueue)
            ->RegisterCallback(bqPlayerBufferQueue, bqPlayerCallback, NULL);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;

    // get the effect send interface
//    bqPlayerEffectSend = NULL;
//    if (0 == bqPlayerSampleRate) {
//        result = (*bqPlayerObject)
//                ->GetInterface(bqPlayerObject, SL_IID_EFFECTSEND,
//                               &bqPlayerEffectSend);
//        assert(SL_RESULT_SUCCESS == result);
//        LOGI("get SL_IID_EFFECTSEND interface success");
//        (void)result;
//    }
//
//#if 0  // mute/solo is not supported for sources that are known to be mono, as
//    // this is
//    // get the mute/solo interface
//    result = (*bqPlayerObject)->GetInterface(bqPlayerObject, SL_IID_MUTESOLO, &bqPlayerMuteSolo);
//    assert(SL_RESULT_SUCCESS == result);
//    (void)result;
//#endif

    // get the volume interface
    result = (*bqPlayerObject)
            ->GetInterface(bqPlayerObject, SL_IID_VOLUME, &bqPlayerVolume);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;


    char inBuffer[BUFFER_SIZE * PLAYER_BUFFER_COUNT] = {0};
    // enqueue another buffer
    result = (*bqPlayerBufferQueue)
            ->Enqueue(bqPlayerBufferQueue, inBuffer, sizeof(inBuffer));

    // the most likely other result is SL_RESULT_BUFFER_INSUFFICIENT,
    // which for this code example would indicate a programming error
    if (SL_RESULT_SUCCESS != result) {
        pthread_mutex_unlock(&audioEngineLock);
    }
    (void)result;

    // set the player's state to playing
    result = (*bqPlayerPlay)->SetPlayState(bqPlayerPlay, SL_PLAYSTATE_PLAYING);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;
}

JNIEXPORT void JNICALL
Java_com_darrenyuan_nativefeedback_OpenSLEngine_stopPlay(JNIEnv *env, jobject thiz) {
    LOGI("stop play");
    SLresult result = (*bqPlayerPlay)->SetPlayState(bqPlayerPlay, SL_PLAYSTATE_STOPPED);
    assert(result == SL_RESULT_SUCCESS);
}

void Java_com_darrenyuan_nativefeedback_OpenSLEngine_shutDown(JNIEnv *env, jobject thiz) {
    LOGI("shutDown");
    // destroy audio recorder object, and invalidate all associated interfaces
    if (recorderObject != NULL) {
        (*recorderObject)->Destroy(recorderObject);
        recorderObject = NULL;
        recorderRecord = NULL;
        recorderBufferQueue = NULL;
    }

    // destroy output mix object, and invalidate all associated interfaces
    if (outputMixObject != NULL) {
        (*outputMixObject)->Destroy(outputMixObject);
        outputMixObject = NULL;
        outputMixEnvironmentalReverb = NULL;
    }

    // destroy engine object, and invalidate all associated interfaces
    if (engineObject != NULL) {
        (*engineObject)->Destroy(engineObject);
        engineObject = NULL;
        engineEngine = NULL;
    }

    if(playerBuffers != NULL){
        LOGI("free playerBuffers");
        free(playerBuffers);
        playerBuffers = NULL;
    }

    inputFs.close();

    pthread_mutex_destroy(&audioEngineLock);
}

#ifdef __cplusplus
}
#endif