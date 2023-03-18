//
// Created by darrenyuan on 2022/11/6.
//
// for native audio
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

#include <assert.h>
#include <android/log.h>
#include <pthread.h>

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

// 5 seconds of recorded audio at 44.1 kHz mono, 16-bit signed little endian
#define RECORDER_FRAMES (44100 * 5)
static short recorderBuffer[RECORDER_FRAMES];
static unsigned recorderSize = 0;

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
    pthread_mutex_unlock(&audioEngineLock);
}

JNIEXPORT jboolean JNICALL
Java_com_darrenyuan_nativefeedback_OpenSLRecorder_createAudioRecorder(JNIEnv *env, jobject thiz)
{
    LOGI("createAudioRecorder");
    createEngine();

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
Java_com_darrenyuan_nativefeedback_OpenSLRecorder_startRecord(JNIEnv *env, jobject thiz)
{
    LOGI("startRecord");
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
Java_com_darrenyuan_nativefeedback_OpenSLRecorder_stopRecord(JNIEnv *env, jobject thiz) {
    LOGI("stopRecord");
    SLresult result;
    result =
            (*recorderRecord)->SetRecordState(recorderRecord, SL_RECORDSTATE_STOPPED);
    if (SL_RESULT_SUCCESS == result) {
        LOGI("stop success");
    }
    pthread_mutex_unlock(&audioEngineLock);
}

void Java_com_darrenyuan_nativefeedback_OpenSLRecorder_shutDown(JNIEnv *env, jobject thiz) {
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

    pthread_mutex_destroy(&audioEngineLock);
}

#ifdef __cplusplus
}
#endif