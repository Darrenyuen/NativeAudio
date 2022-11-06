//
// Created by darrenyuan on 2022/11/6.
//
#include "com_darrenyuan_nativefeedback_MainActivity.h"

extern "C" jstring Java_com_darrenyuan_nativefeedback_MainActivity_nativeHelloWorld
        (JNIEnv *env, jobject) {
    return env->NewStringUTF("Hello Native");
}



