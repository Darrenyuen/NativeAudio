//
// Created by darrenyuan on 2022/6/6.
//
#include "com_darrenyuan_nativefeedback_JniTest.h"

JNIEXPORT jstring JNICALL Java_com_darrenyuan_nativefeedback_JniTest_sayHello
  (JNIEnv *env, jobject obj)
{
    return env->NewStringUTF("我是C++定义的String");
}

