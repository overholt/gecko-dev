/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 *
 * The contents of this file are subject to the Netscape Public License
 * Version 1.0 (the "NPL"); you may not use this file except in
 * compliance with the NPL.  You may obtain a copy of the NPL at
 * http://www.mozilla.org/NPL/
 *
 * Software distributed under the NPL is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the NPL
 * for the specific language governing rights and limitations under the
 * NPL.
 *
 * The Initial Developer of this code under the NPL is Netscape
 * Communications Corporation.  Portions created by Netscape are
 * Copyright (C) 1998 Netscape Communications Corporation.  All Rights
 * Reserved.
 */

/*
 * Proxy JNI implementation.
 */

#include "jni.h"
#include "nsISecureJNI2.h"
#include "nsHashtable.h"
#include "nsVector.h"
#include "plstr.h"

// Method Signature Processing.

static jni_type get_jni_type(char sig)
{
	switch (sig) {
	case 'L':
	case '[':
		return jobject_type;
	case 'Z':
		return jboolean_type;
	case 'B':
		return jbyte_type;
	case 'C':
		return jchar_type;
	case 'S':
		return jshort_type;
	case 'I':
		return jint_type;
	case 'J':
		return jlong_type;
	case 'F':
		return jfloat_type;
	case 'D':
		return jdouble_type;
	case 'V':
		return jvoid_type;
	}
	return jvoid_type;
}

static PRBool get_method_type(const char* sig, PRUint32& arg_count, jni_type*& arg_types, jni_type& return_type)
{
	arg_count = 0;
	if (sig[0] == '(') {
		nsVector vec(10);
		++sig;
		while (*sig != ')' && *sig) {
			char arg_sig = *sig++;
			jni_type arg_type = get_jni_type(arg_sig);
			if (arg_type == jobject_type) {
				// could be an array or an object.
				while (*sig == '[') ++sig;
				if (*sig == 'L') {
					// skip over "className;"
					++sig;
					while (*sig != ';') ++sig;
				}
				// skip over scalar or ';'.
				++sig;
			}
			vec.Add((void*)arg_type);
		}
		arg_count = vec.GetSize();
		arg_types = new jni_type[arg_count];
		for (int index = arg_count - 1; index >= 0; --index)
			arg_types[index] = jni_type(vec.Get(index));
		if (*sig == ')') {
			char return_sig = *++sig;
			return_type = get_jni_type(return_sig);
		}
	}
	return PR_FALSE;
}

class JNIHashKey : public nsHashKey {
public:
	JNIHashKey(void* key) : mKey(key) {}

	virtual PRUint32 HashValue(void) const { return PRUint32(mKey); }
	virtual PRBool Equals(const nsHashKey *aKey) const { return mKey == ((JNIHashKey*)aKey)->mKey; }
	virtual nsHashKey *Clone(void) const { return new JNIHashKey(mKey); }

private:
	void* mKey;
};

struct JNIMember {
	char* mName;
	char* mSignature;
	
	JNIMember(const char* name, const char* sig);
	~JNIMember();
};

JNIMember::JNIMember(const char* name, const char* sig)
	: mName(NULL), mSignature(NULL)
{
	mName = PL_strdup(name);
	mSignature = PL_strdup(sig);
}

JNIMember::~JNIMember()
{
	PL_strfree(mName);
	PL_strfree(mSignature);
}

struct JNIField : JNIMember {
	jfieldID mFieldID;
	jni_type mFieldType;
	
	JNIField(const char* name, const char* sig, jfieldID fieldID);
};

JNIField::JNIField(const char* name, const char* sig, jfieldID fieldID)
	: JNIMember(name, sig), mFieldID(fieldID), mFieldType(get_jni_type(*sig))
{
}

struct JNIMethod : JNIMember {
	jmethodID mMethodID;
	PRUint32 mArgCount;
	jni_type* mArgTypes;
	jni_type mReturnType;
	
	JNIMethod(const char* name, const char* sig, jmethodID methodID);
	~JNIMethod();
	
	jvalue* marshallArgs(va_list args);
};

JNIMethod::JNIMethod(const char* name, const char* sig, jmethodID methodID)
	: JNIMember(name, sig), mMethodID(methodID), mArgCount(0), mArgTypes(NULL), mReturnType(jvoid_type)
{
	get_method_type(sig, mArgCount, mArgTypes, mReturnType);
}

JNIMethod::~JNIMethod()
{
	if (mArgTypes != NULL)
		delete[] mArgTypes;
}

/**
 * Copies an argument list into a jvalue array.
 */
jvalue* JNIMethod::marshallArgs(va_list args)
{
	PRUint32 argCount = mArgCount;
	jni_type* argTypes = mArgTypes;
	jvalue* jargs = new jvalue[argCount];
	if (jargs != NULL) {
		for (int i = 0; i < argCount; i++) {
			switch (argTypes[i]) {
			case jobject_type:
				jargs[i].l = va_arg(args, jobject);
				break;
			case jboolean_type:
				jargs[i].z = va_arg(args, jboolean);
				break;
			case jbyte_type:
				jargs[i].b = va_arg(args, jbyte);
				break;
			case jchar_type:
 				jargs[i].b = va_arg(args, jbyte);
				break;
			case jshort_type:
 				jargs[i].s = va_arg(args, jshort);
				break;
			case jint_type:
 				jargs[i].i = va_arg(args, jint);
				break;
			case jlong_type:
 				jargs[i].j = va_arg(args, jlong);
				break;
			case jfloat_type:
 				jargs[i].f = va_arg(args, jfloat);
				break;
			case jdouble_type:
 				jargs[i].d = va_arg(args, jdouble);
				break;
			}
		}
	}
	return jargs;
}

/**
 * Marshalls a va_list into a jvalue array, and destructor automatically
 * deletes when the args go out of scope.
 */
class MarshalledArgs {
public:
	MarshalledArgs(JNIMethod* forMethod, va_list args) : mArgs(forMethod->marshallArgs(args)) {}
	~MarshalledArgs() { delete[] mArgs; }

	operator jvalue* () { return mArgs; }
	
private:
	jvalue* mArgs;
};

class nsJNIEnv : public JNIEnv {
private:
	static JNINativeInterface_ theFuncs;
	static nsHashtable* theIDTable;
	nsISecureJNI2* mSecureEnv;
	jobject mJavaThread;
	
	static nsJNIEnv& nsJNIEnvRef(JNIEnv* env) { return *(nsJNIEnv*)env; }
	nsISecureJNI2* operator->() { return mSecureEnv; }
	
	static jint JNICALL GetVersion(JNIEnv* env)
	{
		nsJNIEnv& secureEnv = nsJNIEnvRef(env);
		jint version = 0;
		nsresult result =  secureEnv->GetVersion(&version);
		return version;
	}

	static jclass JNICALL DefineClass(JNIEnv *env, const char *name, jobject loader, const jbyte *buf, jsize len)
	{
		nsJNIEnv& secureEnv = nsJNIEnvRef(env);
		jclass outClass = NULL;
		nsresult result = secureEnv->DefineClass(name, loader, buf, len, &outClass);
		return outClass;
	}

	static jclass JNICALL FindClass(JNIEnv *env, const char *name)
	{
		nsJNIEnv& secureEnv = nsJNIEnvRef(env);
		jclass outClass = NULL;
		nsresult result = secureEnv->FindClass(name, &outClass);
		return outClass;
	}

	static jclass JNICALL GetSuperclass(JNIEnv *env, jclass sub)
	{
		nsJNIEnv& secureEnv = nsJNIEnvRef(env);
		jclass outSuper = NULL;
		nsresult result = secureEnv->GetSuperclass(sub, &outSuper);
		return outSuper;
	}

	static jboolean JNICALL IsAssignableFrom(JNIEnv *env, jclass sub, jclass sup)
	{
		nsJNIEnv& secureEnv = nsJNIEnvRef(env);
		jboolean outIsAssignable = FALSE;
		nsresult result = secureEnv->IsAssignableFrom(sub, sup, &outIsAssignable);
		return outIsAssignable;
	}
	
	static jint Throw(JNIEnv *env, jthrowable obj)
	{
		nsJNIEnv& secureEnv = nsJNIEnvRef(env);
		jint outStatus = FALSE;
		nsresult result = secureEnv->Throw(obj, &outStatus);
		return outStatus;
	}
	
	static jint JNICALL ThrowNew(JNIEnv *env, jclass clazz, const char *msg)
	{
		nsJNIEnv& secureEnv = nsJNIEnvRef(env);
		jint outStatus = FALSE;
		nsresult result = secureEnv->ThrowNew(clazz, msg, &outStatus);
		return outStatus;
	}
	
	static jthrowable JNICALL ExceptionOccurred(JNIEnv *env)
	{
		nsJNIEnv& secureEnv = nsJNIEnvRef(env);
		jthrowable outThrowable = NULL;
		nsresult result = secureEnv->ExceptionOccurred(&outThrowable);
		return outThrowable;
	}

	static void JNICALL ExceptionDescribe(JNIEnv *env)
	{
		nsJNIEnv& secureEnv = nsJNIEnvRef(env);
		nsresult result = secureEnv->ExceptionDescribe();
	}
	
	static void JNICALL ExceptionClear(JNIEnv *env)
	{
		nsJNIEnv& secureEnv = nsJNIEnvRef(env);
		nsresult result = secureEnv->ExceptionClear();
	}
	
	static void JNICALL FatalError(JNIEnv *env, const char *msg)
	{
		nsJNIEnv& secureEnv = nsJNIEnvRef(env);
		nsresult result = secureEnv->FatalError(msg);
	}

	static jobject JNICALL NewGlobalRef(JNIEnv *env, jobject lobj)
	{
		nsJNIEnv& secureEnv = nsJNIEnvRef(env);
		jobject outGlobalRef = NULL;
		nsresult result = secureEnv->NewGlobalRef(lobj, &outGlobalRef);
		return outGlobalRef;
	}
	
	static void JNICALL DeleteGlobalRef(JNIEnv *env, jobject gref)
	{
		nsJNIEnv& secureEnv = nsJNIEnvRef(env);
		nsresult result = secureEnv->DeleteGlobalRef(gref);
	}
	
	static void JNICALL DeleteLocalRef(JNIEnv *env, jobject obj)
	{
		nsJNIEnv& secureEnv = nsJNIEnvRef(env);
		nsresult result = secureEnv->DeleteLocalRef(obj);
	}
	
	static jboolean JNICALL IsSameObject(JNIEnv *env, jobject obj1, jobject obj2)
	{
		nsJNIEnv& secureEnv = nsJNIEnvRef(env);
		jboolean outIsSameObject = FALSE;
		nsresult result = secureEnv->IsSameObject(obj1, obj2, &outIsSameObject);
		return outIsSameObject;
	}

	static jobject JNICALL AllocObject(JNIEnv *env, jclass clazz)
	{
		nsJNIEnv& secureEnv = nsJNIEnvRef(env);
		jobject outObject = NULL;
		nsresult result = secureEnv->AllocObject(clazz, &outObject);
		return outObject;
	}
	
	static jobject JNICALL NewObject(JNIEnv *env, jclass clazz, jmethodID methodID, ...)
	{
		va_list args; va_start(args, methodID);
		jobject outObject = NewObjectV(env, clazz, methodID, args);
		va_end(args);
		return outObject;
	}
	
	static jobject JNICALL NewObjectV(JNIEnv *env, jclass clazz, jmethodID methodID, va_list args)
	{
		jobject outObject = NULL;
		
		// convert the va_list into an array of jvalues.
		JNIMethod* method = (JNIMethod*)methodID;
		MarshalledArgs jargs(method, args);
		nsJNIEnv& secureEnv = nsJNIEnvRef(env);
		nsresult result = secureEnv->NewObject(clazz, method->mMethodID, jargs, &outObject);
		
		return outObject;
	}

	static jobject JNICALL NewObjectA(JNIEnv *env, jclass clazz, jmethodID methodID, jvalue *args)
	{
		jobject outObject = NULL;
		nsJNIEnv& secureEnv = nsJNIEnvRef(env);
		JNIMethod* method = (JNIMethod*)methodID;
		nsresult result = secureEnv->NewObject(clazz, method->mMethodID, args, &outObject);
		return outObject;
	}

	static jclass JNICALL GetObjectClass(JNIEnv *env, jobject obj)
	{
		jclass outClass = NULL;
		nsJNIEnv& secureEnv = nsJNIEnvRef(env);
		nsresult result = secureEnv->GetObjectClass(obj, &outClass);
		return outClass;
	}
	
	static jboolean JNICALL IsInstanceOf(JNIEnv *env, jobject obj, jclass clazz)
	{
		nsJNIEnv& secureEnv = nsJNIEnvRef(env);
		jboolean outIsInstanceOf = FALSE;
		nsresult result = secureEnv->IsInstanceOf(obj, clazz, &outIsInstanceOf);
		return outIsInstanceOf;
	}

	static jmethodID JNICALL GetMethodID(JNIEnv *env, jclass clazz, const char *name, const char *sig)
	{
		nsJNIEnv& secureEnv = nsJNIEnvRef(env);
		jmethodID outMethodID = NULL;
		nsresult result = secureEnv->GetMethodID(clazz, name, sig, &outMethodID);
		if (result == NS_OK) {
			JNIHashKey key(outMethodID);
			JNIMethod* method = (JNIMethod*) theIDTable->Get(&key);
			if (method == NULL) {
				method = new JNIMethod(name, sig, outMethodID);
				theIDTable->Put(&key, method);
			}
			outMethodID = jmethodID(method);
		}
		return outMethodID;
	}
	
	/**
	 * Bottleneck methods called by method families below.
	 */
	
	// Virtual Invokers.
	
	static jvalue InvokeMethod(JNIEnv *env, jobject obj, JNIMethod* method, jvalue* args)
	{
		jvalue outValue = { NULL };
		nsJNIEnv& secureEnv = nsJNIEnvRef(env);
		nsresult result = secureEnv->CallMethod(method->mReturnType, obj, method->mMethodID, args, &outValue);
		return outValue;
	}
	
	static jvalue InvokeMethod(JNIEnv *env, jobject obj, JNIMethod* method, va_list args)
	{
		// convert the va_list into an array of jvalues.
		MarshalledArgs jargs(method, args);
		return InvokeMethod(env, obj, method, jargs);
	}
	
	static void InvokeVoidMethod(JNIEnv *env, jobject obj, JNIMethod* method, jvalue* args)
	{
		jvalue unusedValue;
		nsJNIEnv& secureEnv = nsJNIEnvRef(env);
		nsresult result = secureEnv->CallMethod(jvoid_type, obj, method->mMethodID, args, &unusedValue);
	}

	static void InvokeVoidMethod(JNIEnv *env, jobject obj, JNIMethod* method, va_list args)
	{
		// convert the va_list into an array of jvalues.
		MarshalledArgs jargs(method, args);
		InvokeVoidMethod(env, obj, method, jargs);
	}

#define IMPLEMENT_METHOD_FAMILY(methodName, returnType, jvalueField) \
	static returnType JNICALL methodName(JNIEnv *env, jobject obj, jmethodID methodID, ...)					\
	{																										\
		va_list args; va_start(args, methodID);																\
		returnType result = InvokeMethod(env, obj, (JNIMethod*)methodID, args).jvalueField;					\
		va_end(args);																						\
		return result;																						\
	}																										\
																											\
	static returnType JNICALL methodName##V(JNIEnv *env, jobject obj, jmethodID methodID, va_list args)		\
	{																										\
		return InvokeMethod(env, obj, (JNIMethod*)methodID, args).jvalueField;								\
	}																										\
																											\
	static returnType JNICALL methodName##A(JNIEnv *env, jobject obj, jmethodID methodID, jvalue * args)	\
	{																										\
		return InvokeMethod(env, obj, (JNIMethod*)methodID, args).jvalueField;								\
	}																										\

	// Pattern on which IMPLEMENT_METHOD_FAMILY is based.
	/*
	static jobject JNICALL CallObjectMethod(JNIEnv *env, jobject obj, jmethodID methodID, ...)
	{
		va_list args; va_start(args, methodID);
		jobject outObject = InvokeMethod(env, obj, (JNIMethod*)methodID, args).l;
		va_end(args);
		return outObject;
	}
	
	static jobject JNICALL CallObjectMethodV(JNIEnv *env, jobject obj, jmethodID methodID, va_list args)
	{
		return InvokeMethod(env, obj, (JNIMethod*)methodID, args).l;
	}
	
	static jobject JNICALL CallObjectMethodA(JNIEnv *env, jobject obj, jmethodID methodID, jvalue * args)
	{
		return InvokeMethod(env, obj, (JNIMethod*)methodID, args).l;
	}
	*/
	
	IMPLEMENT_METHOD_FAMILY(CallObjectMethod, jobject, l)
	IMPLEMENT_METHOD_FAMILY(CallBooleanMethod, jboolean, z)
	IMPLEMENT_METHOD_FAMILY(CallByteMethod, jbyte, b)
	IMPLEMENT_METHOD_FAMILY(CallCharMethod, jchar, c)
	IMPLEMENT_METHOD_FAMILY(CallShortMethod, jshort, s)
	IMPLEMENT_METHOD_FAMILY(CallIntMethod, jint, i)
	IMPLEMENT_METHOD_FAMILY(CallLongMethod, jlong, j)
	IMPLEMENT_METHOD_FAMILY(CallFloatMethod, jfloat, f)
	IMPLEMENT_METHOD_FAMILY(CallDoubleMethod, jdouble, d)

#undef IMPLEMENT_METHOD_FAMILY

	static void JNICALL CallVoidMethod(JNIEnv *env, jobject obj, jmethodID methodID, ...)
	{
		va_list args; va_start(args, methodID);
		InvokeVoidMethod(env, obj, (JNIMethod*)methodID, args);
		va_end(args);
	}
	
	static void JNICALL CallVoidMethodV(JNIEnv *env, jobject obj, jmethodID methodID, va_list args)
	{
		InvokeVoidMethod(env, obj, (JNIMethod*)methodID, args);
	}
	
	static void JNICALL CallVoidMethodA(JNIEnv *env, jobject obj, jmethodID methodID, jvalue * args)
	{
		InvokeVoidMethod(env, obj, (JNIMethod*)methodID, args);
	}

	// Non-virtual Invokers.

	static jvalue InvokeNonVirtualMethod(JNIEnv *env, jobject obj, jclass clazz, JNIMethod* method, jvalue* args)
	{
		jvalue outValue = { NULL };
		nsJNIEnv& secureEnv = nsJNIEnvRef(env);
		nsresult result = secureEnv->CallNonvirtualMethod(method->mReturnType, obj, clazz, method->mMethodID, args, &outValue);
		return outValue;
	}
	
	static jvalue InvokeNonVirtualMethod(JNIEnv *env, jobject obj, jclass clazz, JNIMethod* method, va_list args)
	{
		// convert the va_list into an array of jvalues.
		MarshalledArgs jargs(method, args);
		return InvokeNonVirtualMethod(env, obj, clazz, method, jargs);
	}
	
	static void InvokeNonVirtualVoidMethod(JNIEnv *env, jobject obj, jclass clazz, JNIMethod* method, jvalue* args)
	{
		jvalue unusedValue;
		nsJNIEnv& secureEnv = nsJNIEnvRef(env);
		nsresult result = secureEnv->CallNonvirtualMethod(jvoid_type, obj, clazz, method->mMethodID, args, &unusedValue);
	}

	static void InvokeNonVirtualVoidMethod(JNIEnv *env, jobject obj, jclass clazz, JNIMethod* method, va_list args)
	{
		// convert the va_list into an array of jvalues.
		MarshalledArgs jargs(method, args);
		InvokeNonVirtualVoidMethod(env, obj, clazz, method, jargs);
	}

#define IMPLEMENT_METHOD_FAMILY(methodName, returnType, jvalueField) \
	static returnType JNICALL methodName(JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, ...)				\
	{																													\
		va_list args; va_start(args, methodID);																			\
		returnType result = InvokeNonVirtualMethod(env, obj, clazz, (JNIMethod*)methodID, args).jvalueField;			\
		va_end(args);																									\
		return result;																									\
	}																													\
																														\
	static returnType JNICALL methodName##V(JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, va_list args)	\
	{																													\
		return InvokeNonVirtualMethod(env, obj, clazz, (JNIMethod*)methodID, args).jvalueField;							\
	}																													\
																														\
	static returnType JNICALL methodName##A(JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, jvalue * args)	\
	{																													\
		return InvokeNonVirtualMethod(env, obj, clazz, (JNIMethod*)methodID, args).jvalueField;							\
	}																													\

	IMPLEMENT_METHOD_FAMILY(CallNonvirtualObjectMethod, jobject, l)
	IMPLEMENT_METHOD_FAMILY(CallNonvirtualBooleanMethod, jboolean, z)
	IMPLEMENT_METHOD_FAMILY(CallNonvirtualByteMethod, jbyte, b)
	IMPLEMENT_METHOD_FAMILY(CallNonvirtualCharMethod, jchar, c)
	IMPLEMENT_METHOD_FAMILY(CallNonvirtualShortMethod, jshort, s)
	IMPLEMENT_METHOD_FAMILY(CallNonvirtualIntMethod, jint, i)
	IMPLEMENT_METHOD_FAMILY(CallNonvirtualLongMethod, jlong, j)
	IMPLEMENT_METHOD_FAMILY(CallNonvirtualFloatMethod, jfloat, f)
	IMPLEMENT_METHOD_FAMILY(CallNonvirtualDoubleMethod, jdouble, d)

#undef IMPLEMENT_METHOD_FAMILY

	static void JNICALL CallNonvirtualVoidMethod(JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, ...)
	{
		va_list args; va_start(args, methodID);
		InvokeNonVirtualVoidMethod(env, obj, clazz, (JNIMethod*)methodID, args);
		va_end(args);
	}
	
	static void JNICALL CallNonvirtualVoidMethodV(JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, va_list args)
	{
		InvokeNonVirtualVoidMethod(env, obj, clazz, (JNIMethod*)methodID, args);
	}
	
	static void JNICALL CallNonvirtualVoidMethodA(JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, jvalue * args)
	{
		InvokeNonVirtualVoidMethod(env, obj, clazz, (JNIMethod*)methodID, args);
	}

	// Instance Fields

	static jfieldID JNICALL GetFieldID(JNIEnv *env, jclass clazz, const char *name, const char *sig)
	{
		nsJNIEnv& secureEnv = nsJNIEnvRef(env);
		jfieldID outFieldID = NULL;
		nsresult result = secureEnv->GetFieldID(clazz, name, sig, &outFieldID);
		if (result == NS_OK) {
			JNIHashKey key(outFieldID);
			JNIField* field = (JNIField*) theIDTable->Get(&key);
			if (field == NULL) {
				field = new JNIField(name, sig, outFieldID);
				theIDTable->Put(&key, field);
			}
			outFieldID = jfieldID(field);
		}
		return outFieldID;
	}

	static jvalue GetField(JNIEnv* env, jobject obj, JNIField* field)
	{
		jvalue outValue = { NULL };
		nsJNIEnv& secureEnv = nsJNIEnvRef(env);
		nsresult result = secureEnv->GetField(field->mFieldType, obj, field->mFieldID, &outValue);
		return outValue;
	}

#define IMPLEMENT_GET_FIELD(methodName, returnType, jvalueField)							\
	static returnType JNICALL methodName(JNIEnv *env, jobject obj, jfieldID fieldID)		\
	{																						\
		return GetField(env, obj, (JNIField*)fieldID).jvalueField;							\
	}																						\

	IMPLEMENT_GET_FIELD(GetObjectField, jobject, l)
	IMPLEMENT_GET_FIELD(GetBooleanField, jboolean, z)
	IMPLEMENT_GET_FIELD(GetByteField, jbyte, b)
	IMPLEMENT_GET_FIELD(GetCharField, jchar, c)
	IMPLEMENT_GET_FIELD(GetShortField, jshort, s)
	IMPLEMENT_GET_FIELD(GetIntField, jint, i)
	IMPLEMENT_GET_FIELD(GetLongField, jlong, j)
	IMPLEMENT_GET_FIELD(GetFloatField, jfloat, f)
	IMPLEMENT_GET_FIELD(GetDoubleField, jdouble, d)

#undef IMPLEMENT_GET_FIELD

	static void SetField(JNIEnv* env, jobject obj, JNIField* field, jvalue value)
	{
		nsJNIEnv& secureEnv = nsJNIEnvRef(env);
		nsresult result = secureEnv->SetField(field->mFieldType, obj, field->mFieldID, value);
	}

#define IMPLEMENT_SET_FIELD(methodName, fieldType, jvalueField)										\
	static void JNICALL methodName(JNIEnv *env, jobject obj, jfieldID fieldID, fieldType value)		\
	{																								\
		jvalue fieldValue;																			\
		fieldValue.jvalueField = value;																\
		SetField(env, obj, (JNIField*)fieldID, fieldValue);											\
	}																								\

	IMPLEMENT_SET_FIELD(SetObjectField, jobject, l)
	IMPLEMENT_SET_FIELD(SetBooleanField, jboolean, z)
	IMPLEMENT_SET_FIELD(SetByteField, jbyte, b)
	IMPLEMENT_SET_FIELD(SetCharField, jchar, c)
	IMPLEMENT_SET_FIELD(SetShortField, jshort, s)
	IMPLEMENT_SET_FIELD(SetIntField, jint, i)
	IMPLEMENT_SET_FIELD(SetLongField, jlong, j)
	IMPLEMENT_SET_FIELD(SetFloatField, jfloat, f)
	IMPLEMENT_SET_FIELD(SetDoubleField, jdouble, d)

#undef IMPLEMENT_SET_FIELD

	// Static Methods

	static jmethodID JNICALL GetStaticMethodID(JNIEnv *env, jclass clazz, const char *name, const char *sig)
	{
		nsJNIEnv& secureEnv = nsJNIEnvRef(env);
		jmethodID outMethodID = NULL;
		nsresult result = secureEnv->GetStaticMethodID(clazz, name, sig, &outMethodID);
		if (result == NS_OK) {
			JNIHashKey key(outMethodID);
			JNIMethod* method = (JNIMethod*) theIDTable->Get(&key);
			if (method == NULL) {
				method = new JNIMethod(name, sig, outMethodID);
				theIDTable->Put(&key, method);
			}
			outMethodID = jmethodID(method);
		}
		return outMethodID;
	}

	// jobject (JNICALL *CallStaticObjectMethod) (JNIEnv *env, jclass clazz, jmethodID methodID, ...);
	
	// Static Invokers.
	
	static jvalue InvokeStaticMethod(JNIEnv *env, jclass clazz, JNIMethod* method, jvalue* args)
	{
		jvalue outValue = { NULL };
		nsJNIEnv& secureEnv = nsJNIEnvRef(env);
		nsresult result = secureEnv->CallStaticMethod(method->mReturnType, clazz, method->mMethodID, args, &outValue);
		return outValue;
	}
	
	static jvalue InvokeStaticMethod(JNIEnv *env, jclass clazz, JNIMethod* method, va_list args)
	{
		// convert the va_list into an array of jvalues.
		MarshalledArgs jargs(method, args);
		return InvokeStaticMethod(env, clazz, method, jargs);
	}
	
	static void InvokeStaticVoidMethod(JNIEnv *env, jclass clazz, JNIMethod* method, jvalue* args)
	{
		jvalue unusedValue;
		nsJNIEnv& secureEnv = nsJNIEnvRef(env);
		nsresult result = secureEnv->CallStaticMethod(jvoid_type, clazz, method->mMethodID, args, &unusedValue);
	}

	static void InvokeStaticVoidMethod(JNIEnv *env, jclass clazz, JNIMethod* method, va_list args)
	{
		// convert the va_list into an array of jvalues.
		MarshalledArgs jargs(method, args);
		InvokeStaticVoidMethod(env, clazz, method, jargs);
	}

#define IMPLEMENT_METHOD_FAMILY(methodName, returnType, jvalueField) \
	static returnType JNICALL methodName(JNIEnv *env, jclass clazz, jmethodID methodID, ...)				\
	{																										\
		va_list args; va_start(args, methodID);																\
		returnType result = InvokeStaticMethod(env, clazz, (JNIMethod*)methodID, args).jvalueField;			\
		va_end(args);																						\
		return result;																						\
	}																										\
																											\
	static returnType JNICALL methodName##V(JNIEnv *env, jclass clazz, jmethodID methodID, va_list args)	\
	{																										\
		return InvokeStaticMethod(env, clazz, (JNIMethod*)methodID, args).jvalueField;						\
	}																										\
																											\
	static returnType JNICALL methodName##A(JNIEnv *env, jclass clazz, jmethodID methodID, jvalue * args)	\
	{																										\
		return InvokeStaticMethod(env, clazz, (JNIMethod*)methodID, args).jvalueField;						\
	}																										\

	IMPLEMENT_METHOD_FAMILY(CallStaticObjectMethod, jobject, l)
	IMPLEMENT_METHOD_FAMILY(CallStaticBooleanMethod, jboolean, z)
	IMPLEMENT_METHOD_FAMILY(CallStaticByteMethod, jbyte, b)
	IMPLEMENT_METHOD_FAMILY(CallStaticCharMethod, jchar, c)
	IMPLEMENT_METHOD_FAMILY(CallStaticShortMethod, jshort, s)
	IMPLEMENT_METHOD_FAMILY(CallStaticIntMethod, jint, i)
	IMPLEMENT_METHOD_FAMILY(CallStaticLongMethod, jlong, j)
	IMPLEMENT_METHOD_FAMILY(CallStaticFloatMethod, jfloat, f)
	IMPLEMENT_METHOD_FAMILY(CallStaticDoubleMethod, jdouble, d)

#undef IMPLEMENT_METHOD_FAMILY

	static void JNICALL CallStaticVoidMethod(JNIEnv *env, jclass clazz, jmethodID methodID, ...)
	{
		va_list args; va_start(args, methodID);
		InvokeStaticVoidMethod(env, clazz, (JNIMethod*)methodID, args);
		va_end(args);
	}
	
	static void JNICALL CallStaticVoidMethodV(JNIEnv *env, jclass clazz, jmethodID methodID, va_list args)
	{
		InvokeStaticVoidMethod(env, clazz, (JNIMethod*)methodID, args);
	}
	
	static void JNICALL CallStaticVoidMethodA(JNIEnv *env, jclass clazz, jmethodID methodID, jvalue * args)
	{
		InvokeStaticVoidMethod(env, clazz, (JNIMethod*)methodID, args);
	}

	// Static Fields

	static jfieldID JNICALL GetStaticFieldID(JNIEnv *env, jclass clazz, const char *name, const char *sig)
	{
		nsJNIEnv& secureEnv = nsJNIEnvRef(env);
		jfieldID outFieldID = NULL;
		nsresult result = secureEnv->GetStaticFieldID(clazz, name, sig, &outFieldID);
		if (result == NS_OK) {
			JNIHashKey key(outFieldID);
			JNIField* field = (JNIField*) theIDTable->Get(&key);
			if (field == NULL) {
				field = new JNIField(name, sig, outFieldID);
				theIDTable->Put(&key, field);
			}
			outFieldID = jfieldID(field);
		}
		return outFieldID;
	}

	static jvalue GetStaticField(JNIEnv* env, jclass clazz, JNIField* field)
	{
		jvalue outValue = { NULL };
		nsJNIEnv& secureEnv = nsJNIEnvRef(env);
		nsresult result = secureEnv->GetStaticField(field->mFieldType, clazz, field->mFieldID, &outValue);
		return outValue;
	}

#define IMPLEMENT_GET_FIELD(methodName, returnType, jvalueField)							\
	static returnType JNICALL methodName(JNIEnv *env, jclass clazz, jfieldID fieldID)		\
	{																						\
		return GetStaticField(env, clazz, (JNIField*)fieldID).jvalueField;					\
	}																						\

	IMPLEMENT_GET_FIELD(GetStaticObjectField, jobject, l)
	IMPLEMENT_GET_FIELD(GetStaticBooleanField, jboolean, z)
	IMPLEMENT_GET_FIELD(GetStaticByteField, jbyte, b)
	IMPLEMENT_GET_FIELD(GetStaticCharField, jchar, c)
	IMPLEMENT_GET_FIELD(GetStaticShortField, jshort, s)
	IMPLEMENT_GET_FIELD(GetStaticIntField, jint, i)
	IMPLEMENT_GET_FIELD(GetStaticLongField, jlong, j)
	IMPLEMENT_GET_FIELD(GetStaticFloatField, jfloat, f)
	IMPLEMENT_GET_FIELD(GetStaticDoubleField, jdouble, d)

#undef IMPLEMENT_GET_FIELD

	static void SetStaticField(JNIEnv* env, jclass clazz, JNIField* field, jvalue value)
	{
		nsJNIEnv& secureEnv = nsJNIEnvRef(env);
		nsresult result = secureEnv->SetStaticField(field->mFieldType, clazz, field->mFieldID, value);
	}

#define IMPLEMENT_SET_FIELD(methodName, fieldType, jvalueField)										\
	static void JNICALL methodName(JNIEnv *env, jclass clazz, jfieldID fieldID, fieldType value)	\
	{																								\
		jvalue fieldValue;																			\
		fieldValue.jvalueField = value;																\
		SetStaticField(env, clazz, (JNIField*)fieldID, fieldValue);									\
	}																								\

	IMPLEMENT_SET_FIELD(SetStaticObjectField, jobject, l)
	IMPLEMENT_SET_FIELD(SetStaticBooleanField, jboolean, z)
	IMPLEMENT_SET_FIELD(SetStaticByteField, jbyte, b)
	IMPLEMENT_SET_FIELD(SetStaticCharField, jchar, c)
	IMPLEMENT_SET_FIELD(SetStaticShortField, jshort, s)
	IMPLEMENT_SET_FIELD(SetStaticIntField, jint, i)
	IMPLEMENT_SET_FIELD(SetStaticLongField, jlong, j)
	IMPLEMENT_SET_FIELD(SetStaticFloatField, jfloat, f)
	IMPLEMENT_SET_FIELD(SetStaticDoubleField, jdouble, d)

#undef IMPLEMENT_SET_FIELD

	static jstring JNICALL NewString(JNIEnv *env, const jchar *unicode, jsize len)
	{
		jstring outString;
		nsJNIEnv& secureEnv = nsJNIEnvRef(env);
		nsresult result = secureEnv->NewString(unicode, len, &outString);
		return outString;
	}
	
	static jsize JNICALL GetStringLength(JNIEnv *env, jstring str)
	{
		jsize outLength;
		nsJNIEnv& secureEnv = nsJNIEnvRef(env);
		nsresult result = secureEnv->GetStringLength(str, &outLength);
		return outLength;
	}
	
	static const jchar* JNICALL GetStringChars(JNIEnv *env, jstring str, jboolean *isCopy)
	{
		jchar* outChars = NULL;
		nsJNIEnv& secureEnv = nsJNIEnvRef(env);
		nsresult result = secureEnv->GetStringChars(str, isCopy, &outChars);
		return outChars;
	}
	
	static void JNICALL ReleaseStringChars(JNIEnv *env, jstring str, const jchar *chars)
	{
		nsJNIEnv& secureEnv = nsJNIEnvRef(env);
		nsresult result = secureEnv->ReleaseStringChars(str, chars);
	}

	// jstring (JNICALL *NewStringUTF) (JNIEnv *env, const char *utf);
	// jsize (JNICALL *GetStringUTFLength) (JNIEnv *env, jstring str);
	// const char* (JNICALL *GetStringUTFChars) (JNIEnv *env, jstring str, jboolean *isCopy);
	// void (JNICALL *ReleaseStringUTFChars) (JNIEnv *env, jstring str, const char* chars);

public:
	nsJNIEnv(nsISecureJNI2* secureEnv);
	virtual ~nsJNIEnv();
};

JNINativeInterface_ nsJNIEnv::theFuncs = {
	NULL,	// void *reserved0;
	NULL,	// void *reserved1;
	NULL,	// void *reserved2;

	NULL,	// void *reserved3;

	// jint (JNICALL *GetVersion)(JNIEnv *env);	
	&GetVersion,

	// jclass (JNICALL *DefineClass) (JNIEnv *env, const char *name, jobject loader, const jbyte *buf, jsize len);
	&DefineClass,
	
	// jclass (JNICALL *FindClass) (JNIEnv *env, const char *name);
	&FindClass,

	NULL,	// void *reserved4;
	NULL,	// void *reserved5;
	NULL,	// void *reserved6;

	// jclass (JNICALL *GetSuperclass) (JNIEnv *env, jclass sub);
	&GetSuperclass,
	
	// jboolean (JNICALL *IsAssignableFrom) (JNIEnv *env, jclass sub, jclass sup);
	&IsAssignableFrom,
	
	NULL, // void *reserved7;

	// jint (JNICALL *Throw) (JNIEnv *env, jthrowable obj);
	&Throw,
	
	// jint (JNICALL *ThrowNew) (JNIEnv *env, jclass clazz, const char *msg);
	&ThrowNew,
	
	// jthrowable (JNICALL *ExceptionOccurred) (JNIEnv *env);
	&ExceptionOccurred,
	
	// void (JNICALL *ExceptionDescribe) (JNIEnv *env);
	&ExceptionDescribe,
	
	// void (JNICALL *ExceptionClear) (JNIEnv *env);
	&ExceptionClear,
	
	// void (JNICALL *FatalError) (JNIEnv *env, const char *msg);
	&FatalError,
	
	NULL, // void *reserved8;
	NULL, // void *reserved9;

	// jobject (JNICALL *NewGlobalRef) (JNIEnv *env, jobject lobj);
	&NewGlobalRef,
	
	// void (JNICALL *DeleteGlobalRef) (JNIEnv *env, jobject gref);
	&DeleteGlobalRef,
	
	// void (JNICALL *DeleteLocalRef) (JNIEnv *env, jobject obj);
	&DeleteLocalRef,
	
	// jboolean (JNICALL *IsSameObject) (JNIEnv *env, jobject obj1, jobject obj2);
	&IsSameObject,
	
	NULL, // void *reserved10;
	NULL, // void *reserved11;

	// jobject (JNICALL *AllocObject) (JNIEnv *env, jclass clazz);
	&AllocObject,
	
#define REFERENCE_METHOD_FAMILY(methodName) &methodName, &methodName##V, &methodName##A,

	// jobject (JNICALL *NewObject) (JNIEnv *env, jclass clazz, jmethodID methodID, ...);
	// jobject (JNICALL *NewObjectV) (JNIEnv *env, jclass clazz, jmethodID methodID, va_list args);
	// jobject (JNICALL *NewObjectA) (JNIEnv *env, jclass clazz, jmethodID methodID, jvalue *args);
	REFERENCE_METHOD_FAMILY(NewObject)
	
	// jclass (JNICALL *GetObjectClass) (JNIEnv *env, jobject obj);
	&GetObjectClass,
	
	// jboolean (JNICALL *IsInstanceOf) (JNIEnv *env, jobject obj, jclass clazz);
	&IsInstanceOf,

	// jmethodID (JNICALL *GetMethodID)(JNIEnv *env, jclass clazz, const char *name, const char *sig);
	&GetMethodID,

	// jobject (JNICALL *CallObjectMethod) (JNIEnv *env, jobject obj, jmethodID methodID, ...);
	// jobject (JNICALL *CallObjectMethodV) (JNIEnv *env, jobject obj, jmethodID methodID, va_list args);
	// jobject (JNICALL *CallObjectMethodA) (JNIEnv *env, jobject obj, jmethodID methodID, jvalue * args);
	REFERENCE_METHOD_FAMILY(CallObjectMethod)
	
	// jboolean (JNICALL *CallBooleanMethod) (JNIEnv *env, jobject obj, jmethodID methodID, ...);
	// jboolean (JNICALL *CallBooleanMethodV) (JNIEnv *env, jobject obj, jmethodID methodID, va_list args);
	// jboolean (JNICALL *CallBooleanMethodA) (JNIEnv *env, jobject obj, jmethodID methodID, jvalue * args);
	REFERENCE_METHOD_FAMILY(CallBooleanMethod)

	// jbyte (JNICALL *CallByteMethod) (JNIEnv *env, jobject obj, jmethodID methodID, ...);
	// jbyte (JNICALL *CallByteMethodV) (JNIEnv *env, jobject obj, jmethodID methodID, va_list args);
	// jbyte (JNICALL *CallByteMethodA) (JNIEnv *env, jobject obj, jmethodID methodID, jvalue *args);
	REFERENCE_METHOD_FAMILY(CallByteMethod)

	// jchar (JNICALL *CallCharMethod) (JNIEnv *env, jobject obj, jmethodID methodID, ...);
	// jchar (JNICALL *CallCharMethodV) (JNIEnv *env, jobject obj, jmethodID methodID, va_list args);
	// jchar (JNICALL *CallCharMethodA) (JNIEnv *env, jobject obj, jmethodID methodID, jvalue *args);
	REFERENCE_METHOD_FAMILY(CallCharMethod)

	// jshort (JNICALL *CallShortMethod) (JNIEnv *env, jobject obj, jmethodID methodID, ...);
	// jshort (JNICALL *CallShortMethodV) (JNIEnv *env, jobject obj, jmethodID methodID, va_list args);
	// jshort (JNICALL *CallShortMethodA) (JNIEnv *env, jobject obj, jmethodID methodID, jvalue *args);
	REFERENCE_METHOD_FAMILY(CallShortMethod)

	// jint (JNICALL *CallIntMethod) (JNIEnv *env, jobject obj, jmethodID methodID, ...);
	// jint (JNICALL *CallIntMethodV) (JNIEnv *env, jobject obj, jmethodID methodID, va_list args);
	// jint (JNICALL *CallIntMethodA) (JNIEnv *env, jobject obj, jmethodID methodID, jvalue *args);
	REFERENCE_METHOD_FAMILY(CallIntMethod)

	// jlong (JNICALL *CallLongMethod) (JNIEnv *env, jobject obj, jmethodID methodID, ...);
	// jlong (JNICALL *CallLongMethodV) (JNIEnv *env, jobject obj, jmethodID methodID, va_list args);
	// jlong (JNICALL *CallLongMethodA) (JNIEnv *env, jobject obj, jmethodID methodID, jvalue *args);
	REFERENCE_METHOD_FAMILY(CallLongMethod)

	// jfloat (JNICALL *CallFloatMethod) (JNIEnv *env, jobject obj, jmethodID methodID, ...);
	// jfloat (JNICALL *CallFloatMethodV) (JNIEnv *env, jobject obj, jmethodID methodID, va_list args);
	// jfloat (JNICALL *CallFloatMethodA) (JNIEnv *env, jobject obj, jmethodID methodID, jvalue *args);
	REFERENCE_METHOD_FAMILY(CallFloatMethod)

	// jdouble (JNICALL *CallDoubleMethod) (JNIEnv *env, jobject obj, jmethodID methodID, ...);
	// jdouble (JNICALL *CallDoubleMethodV) (JNIEnv *env, jobject obj, jmethodID methodID, va_list args);
	// jdouble (JNICALL *CallDoubleMethodA) (JNIEnv *env, jobject obj, jmethodID methodID, jvalue *args);
	REFERENCE_METHOD_FAMILY(CallDoubleMethod)

	// void (JNICALL *CallVoidMethod) (JNIEnv *env, jobject obj, jmethodID methodID, ...);
	// void (JNICALL *CallVoidMethodV) (JNIEnv *env, jobject obj, jmethodID methodID, va_list args);
	// void (JNICALL *CallVoidMethodA) (JNIEnv *env, jobject obj, jmethodID methodID, jvalue * args);
	REFERENCE_METHOD_FAMILY(CallVoidMethod)

	// jobject (JNICALL *CallNonvirtualObjectMethod) (JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, ...);
	// jobject (JNICALL *CallNonvirtualObjectMethodV) (JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, va_list args);
	// jobject (JNICALL *CallNonvirtualObjectMethodA) (JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, jvalue * args);
	REFERENCE_METHOD_FAMILY(CallNonvirtualObjectMethod)

	// jboolean (JNICALL *CallNonvirtualBooleanMethod) (JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, ...);
	// jboolean (JNICALL *CallNonvirtualBooleanMethodV) (JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, va_list args);
	// jboolean (JNICALL *CallNonvirtualBooleanMethodA) (JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, jvalue * args);
	REFERENCE_METHOD_FAMILY(CallNonvirtualBooleanMethod)

	// jbyte (JNICALL *CallNonvirtualByteMethod) (JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, ...);
	// jbyte (JNICALL *CallNonvirtualByteMethodV) (JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, va_list args);
	// jbyte (JNICALL *CallNonvirtualByteMethodA) (JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, jvalue *args);
	REFERENCE_METHOD_FAMILY(CallNonvirtualByteMethod)

	// jchar (JNICALL *CallNonvirtualCharMethod) (JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, ...);
	// jchar (JNICALL *CallNonvirtualCharMethodV) (JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, va_list args);
	// jchar (JNICALL *CallNonvirtualCharMethodA) (JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, jvalue *args);
	REFERENCE_METHOD_FAMILY(CallNonvirtualCharMethod)

	// jshort (JNICALL *CallNonvirtualShortMethod) (JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, ...);
	// jshort (JNICALL *CallNonvirtualShortMethodV) (JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, va_list args);
	// jshort (JNICALL *CallNonvirtualShortMethodA) (JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, jvalue *args);
	REFERENCE_METHOD_FAMILY(CallNonvirtualShortMethod)

	// jint (JNICALL *CallNonvirtualIntMethod) (JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, ...);
	// jint (JNICALL *CallNonvirtualIntMethodV) (JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, va_list args);
	// jint (JNICALL *CallNonvirtualIntMethodA) (JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, jvalue *args);
	REFERENCE_METHOD_FAMILY(CallNonvirtualIntMethod)

	// jlong (JNICALL *CallNonvirtualLongMethod) (JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, ...);
	// jlong (JNICALL *CallNonvirtualLongMethodV) (JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, va_list args);
	// jlong (JNICALL *CallNonvirtualLongMethodA) (JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, jvalue *args);
	REFERENCE_METHOD_FAMILY(CallNonvirtualLongMethod)

	// jfloat (JNICALL *CallNonvirtualFloatMethod) (JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, ...);
	// jfloat (JNICALL *CallNonvirtualFloatMethodV) (JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, va_list args);
	// jfloat (JNICALL *CallNonvirtualFloatMethodA) (JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, jvalue *args);
	REFERENCE_METHOD_FAMILY(CallNonvirtualFloatMethod)

	// jdouble (JNICALL *CallNonvirtualDoubleMethod) (JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, ...);
	// jdouble (JNICALL *CallNonvirtualDoubleMethodV) (JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, va_list args);
	// jdouble (JNICALL *CallNonvirtualDoubleMethodA) (JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, jvalue *args);
	REFERENCE_METHOD_FAMILY(CallNonvirtualDoubleMethod)

	// void (JNICALL *CallNonvirtualVoidMethod) (JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, ...);
	// void (JNICALL *CallNonvirtualVoidMethodV) (JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, va_list args);
	// void (JNICALL *CallNonvirtualVoidMethodA) (JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, jvalue * args);
	REFERENCE_METHOD_FAMILY(CallNonvirtualVoidMethod)

	// jfieldID (JNICALL *GetFieldID) (JNIEnv *env, jclass clazz, const char *name, const char *sig);
	&GetFieldID,

	// jobject (JNICALL *GetObjectField) (JNIEnv *env, jobject obj, jfieldID fieldID);
	// jboolean (JNICALL *GetBooleanField) (JNIEnv *env, jobject obj, jfieldID fieldID);
	// jbyte (JNICALL *GetByteField) (JNIEnv *env, jobject obj, jfieldID fieldID);
	// jchar (JNICALL *GetCharField) (JNIEnv *env, jobject obj, jfieldID fieldID);
	// jshort (JNICALL *GetShortField) (JNIEnv *env, jobject obj, jfieldID fieldID);
	// jint (JNICALL *GetIntField) (JNIEnv *env, jobject obj, jfieldID fieldID);
	// jlong (JNICALL *GetLongField) (JNIEnv *env, jobject obj, jfieldID fieldID);
	// jfloat (JNICALL *GetFloatField) (JNIEnv *env, jobject obj, jfieldID fieldID);
	// jdouble (JNICALL *GetDoubleField) (JNIEnv *env, jobject obj, jfieldID fieldID);
	&GetObjectField, &GetBooleanField, &GetByteField, &GetCharField, &GetShortField, &GetIntField, &GetLongField,
	&GetFloatField, &GetDoubleField,

	// void (JNICALL *SetObjectField) (JNIEnv *env, jobject obj, jfieldID fieldID, jobject val);
	// void (JNICALL *SetBooleanField) (JNIEnv *env, jobject obj, jfieldID fieldID, jboolean val);
	// void (JNICALL *SetByteField) (JNIEnv *env, jobject obj, jfieldID fieldID, jbyte val);
	// void (JNICALL *SetCharField) (JNIEnv *env, jobject obj, jfieldID fieldID, jchar val);
	// void (JNICALL *SetShortField) (JNIEnv *env, jobject obj, jfieldID fieldID, jshort val);
	// void (JNICALL *SetIntField) (JNIEnv *env, jobject obj, jfieldID fieldID, jint val);
	// void (JNICALL *SetLongField) (JNIEnv *env, jobject obj, jfieldID fieldID, jlong val);
	// void (JNICALL *SetFloatField) (JNIEnv *env, jobject obj, jfieldID fieldID, jfloat val);
	// void (JNICALL *SetDoubleField) (JNIEnv *env, jobject obj, jfieldID fieldID, jdouble val);
	&SetObjectField, &SetBooleanField, &SetByteField, &SetCharField, &SetShortField, &SetIntField, &SetLongField,
	&SetFloatField, &SetDoubleField,

	// jmethodID (JNICALL *GetStaticMethodID) (JNIEnv *env, jclass clazz, const char *name, const char *sig);
	&GetStaticMethodID,

	// jobject (JNICALL *CallStaticObjectMethod) (JNIEnv *env, jclass clazz, jmethodID methodID, ...);
	// jobject (JNICALL *CallStaticObjectMethodV) (JNIEnv *env, jclass clazz, jmethodID methodID, va_list args);
	// jobject (JNICALL *CallStaticObjectMethodA) (JNIEnv *env, jclass clazz, jmethodID methodID, jvalue *args);
	REFERENCE_METHOD_FAMILY(CallStaticObjectMethod)

	// jboolean (JNICALL *CallStaticBooleanMethod)
	// (JNIEnv *env, jclass clazz, jmethodID methodID, ...);
	// jboolean (JNICALL *CallStaticBooleanMethodV)
	// (JNIEnv *env, jclass clazz, jmethodID methodID, va_list args);
	// jboolean (JNICALL *CallStaticBooleanMethodA)
	// (JNIEnv *env, jclass clazz, jmethodID methodID, jvalue *args);
	REFERENCE_METHOD_FAMILY(CallStaticBooleanMethod)

	// jbyte (JNICALL *CallStaticByteMethod)
	// (JNIEnv *env, jclass clazz, jmethodID methodID, ...);
	// jbyte (JNICALL *CallStaticByteMethodV)
	// (JNIEnv *env, jclass clazz, jmethodID methodID, va_list args);
	// jbyte (JNICALL *CallStaticByteMethodA)
	// (JNIEnv *env, jclass clazz, jmethodID methodID, jvalue *args);
	REFERENCE_METHOD_FAMILY(CallStaticByteMethod)

	// jchar (JNICALL *CallStaticCharMethod)
	// (JNIEnv *env, jclass clazz, jmethodID methodID, ...);
	// jchar (JNICALL *CallStaticCharMethodV)
	// (JNIEnv *env, jclass clazz, jmethodID methodID, va_list args);
	// jchar (JNICALL *CallStaticCharMethodA)
	// (JNIEnv *env, jclass clazz, jmethodID methodID, jvalue *args);
	REFERENCE_METHOD_FAMILY(CallStaticCharMethod)

	// jshort (JNICALL *CallStaticShortMethod)
	// (JNIEnv *env, jclass clazz, jmethodID methodID, ...);
	// jshort (JNICALL *CallStaticShortMethodV)
	// (JNIEnv *env, jclass clazz, jmethodID methodID, va_list args);
	// jshort (JNICALL *CallStaticShortMethodA)
	// (JNIEnv *env, jclass clazz, jmethodID methodID, jvalue *args);
	REFERENCE_METHOD_FAMILY(CallStaticShortMethod)

	// jint (JNICALL *CallStaticIntMethod)
	// (JNIEnv *env, jclass clazz, jmethodID methodID, ...);
	// jint (JNICALL *CallStaticIntMethodV)
	// (JNIEnv *env, jclass clazz, jmethodID methodID, va_list args);
	// jint (JNICALL *CallStaticIntMethodA)
	// (JNIEnv *env, jclass clazz, jmethodID methodID, jvalue *args);
	REFERENCE_METHOD_FAMILY(CallStaticIntMethod)

	// jlong (JNICALL *CallStaticLongMethod)
	// (JNIEnv *env, jclass clazz, jmethodID methodID, ...);
	// jlong (JNICALL *CallStaticLongMethodV)
	// (JNIEnv *env, jclass clazz, jmethodID methodID, va_list args);
	// jlong (JNICALL *CallStaticLongMethodA)
	// (JNIEnv *env, jclass clazz, jmethodID methodID, jvalue *args);
	REFERENCE_METHOD_FAMILY(CallStaticLongMethod)

	// jfloat (JNICALL *CallStaticFloatMethod)
	// (JNIEnv *env, jclass clazz, jmethodID methodID, ...);
	// jfloat (JNICALL *CallStaticFloatMethodV)
	// (JNIEnv *env, jclass clazz, jmethodID methodID, va_list args);
	// jfloat (JNICALL *CallStaticFloatMethodA)
	// (JNIEnv *env, jclass clazz, jmethodID methodID, jvalue *args);
	REFERENCE_METHOD_FAMILY(CallStaticFloatMethod)

	// jdouble (JNICALL *CallStaticDoubleMethod)
	// (JNIEnv *env, jclass clazz, jmethodID methodID, ...);
	// jdouble (JNICALL *CallStaticDoubleMethodV)
	// (JNIEnv *env, jclass clazz, jmethodID methodID, va_list args);
	// jdouble (JNICALL *CallStaticDoubleMethodA)       
	// (JNIEnv *env, jclass clazz, jmethodID methodID, jvalue *args);
	REFERENCE_METHOD_FAMILY(CallStaticDoubleMethod)

	// void (JNICALL *CallStaticVoidMethod)
	// (JNIEnv *env, jclass cls, jmethodID methodID, ...);
	// void (JNICALL *CallStaticVoidMethodV)
	// (JNIEnv *env, jclass cls, jmethodID methodID, va_list args);
	// void (JNICALL *CallStaticVoidMethodA)
	// (JNIEnv *env, jclass cls, jmethodID methodID, jvalue * args);
	REFERENCE_METHOD_FAMILY(CallStaticVoidMethod)

	// jfieldID (JNICALL *GetStaticFieldID) (JNIEnv *env, jclass clazz, const char *name, const char *sig);
	&GetStaticFieldID,
	
	// jobject (JNICALL *GetStaticObjectField)
	// (JNIEnv *env, jclass clazz, jfieldID fieldID);
	// jboolean (JNICALL *GetStaticBooleanField)
	// (JNIEnv *env, jclass clazz, jfieldID fieldID);
	// jbyte (JNICALL *GetStaticByteField)
	// (JNIEnv *env, jclass clazz, jfieldID fieldID);
	// jchar (JNICALL *GetStaticCharField)
	// (JNIEnv *env, jclass clazz, jfieldID fieldID);
	// jshort (JNICALL *GetStaticShortField)
	// (JNIEnv *env, jclass clazz, jfieldID fieldID);
	// jint (JNICALL *GetStaticIntField)
	// (JNIEnv *env, jclass clazz, jfieldID fieldID);
	// jlong (JNICALL *GetStaticLongField)
	// (JNIEnv *env, jclass clazz, jfieldID fieldID);
	// jfloat (JNICALL *GetStaticFloatField)
	// (JNIEnv *env, jclass clazz, jfieldID fieldID);
	// jdouble (JNICALL *GetStaticDoubleField)
	// (JNIEnv *env, jclass clazz, jfieldID fieldID);
	&GetStaticObjectField, &GetStaticBooleanField, &GetStaticByteField, &GetStaticCharField, &GetStaticShortField,
	&GetStaticIntField, &GetStaticLongField, &GetStaticFloatField, &GetStaticDoubleField,

	// void (JNICALL *SetStaticObjectField)
	// (JNIEnv *env, jclass clazz, jfieldID fieldID, jobject value);
	// void (JNICALL *SetStaticBooleanField)
	// (JNIEnv *env, jclass clazz, jfieldID fieldID, jboolean value);
	// void (JNICALL *SetStaticByteField)
	// (JNIEnv *env, jclass clazz, jfieldID fieldID, jbyte value);
	// void (JNICALL *SetStaticCharField)
	// (JNIEnv *env, jclass clazz, jfieldID fieldID, jchar value);
	// void (JNICALL *SetStaticShortField)
	// (JNIEnv *env, jclass clazz, jfieldID fieldID, jshort value);
	// void (JNICALL *SetStaticIntField)
	// (JNIEnv *env, jclass clazz, jfieldID fieldID, jint value);
	// void (JNICALL *SetStaticLongField)
	// (JNIEnv *env, jclass clazz, jfieldID fieldID, jlong value);
	// void (JNICALL *SetStaticFloatField)
	// (JNIEnv *env, jclass clazz, jfieldID fieldID, jfloat value);
	// void (JNICALL *SetStaticDoubleField)
	// (JNIEnv *env, jclass clazz, jfieldID fieldID, jdouble value);
	&SetStaticObjectField, &SetStaticBooleanField, &SetStaticByteField, &SetStaticCharField, &SetStaticShortField,
	&SetStaticIntField, &SetStaticLongField, &SetStaticFloatField, &SetStaticDoubleField,

	// jstring (JNICALL *NewString) (JNIEnv *env, const jchar *unicode, jsize len);
	// jsize (JNICALL *GetStringLength) (JNIEnv *env, jstring str);
	// const jchar *(JNICALL *GetStringChars) (JNIEnv *env, jstring str, jboolean *isCopy);
	// void (JNICALL *ReleaseStringChars) (JNIEnv *env, jstring str, const jchar *chars);
	&NewString, &GetStringLength, &GetStringChars, &ReleaseStringChars,

	// jstring (JNICALL *NewStringUTF) (JNIEnv *env, const char *utf);
	// jsize (JNICALL *GetStringUTFLength) (JNIEnv *env, jstring str);
	// const char* (JNICALL *GetStringUTFChars) (JNIEnv *env, jstring str, jboolean *isCopy);
	// void (JNICALL *ReleaseStringUTFChars) (JNIEnv *env, jstring str, const char* chars);


	// jsize (JNICALL *GetArrayLength) (JNIEnv *env, jarray array);

	// jobjectArray (JNICALL *NewObjectArray)
	// (JNIEnv *env, jsize len, jclass clazz, jobject init);
	// jobject (JNICALL *GetObjectArrayElement)
	// (JNIEnv *env, jobjectArray array, jsize index);
	// void (JNICALL *SetObjectArrayElement)
	// (JNIEnv *env, jobjectArray array, jsize index, jobject val);

	// jbooleanArray (JNICALL *NewBooleanArray)
	// (JNIEnv *env, jsize len);
	// jbyteArray (JNICALL *NewByteArray)
	// (JNIEnv *env, jsize len);
	// jcharArray (JNICALL *NewCharArray)
	// (JNIEnv *env, jsize len);
	// jshortArray (JNICALL *NewShortArray)
	// (JNIEnv *env, jsize len);
	// jintArray (JNICALL *NewIntArray)
	// (JNIEnv *env, jsize len);
	// jlongArray (JNICALL *NewLongArray)
	// (JNIEnv *env, jsize len);
	// jfloatArray (JNICALL *NewFloatArray)
	// (JNIEnv *env, jsize len);
	// jdoubleArray (JNICALL *NewDoubleArray)
	// (JNIEnv *env, jsize len);

	// jboolean * (JNICALL *GetBooleanArrayElements)
	// (JNIEnv *env, jbooleanArray array, jboolean *isCopy);
	// jbyte * (JNICALL *GetByteArrayElements)
	// (JNIEnv *env, jbyteArray array, jboolean *isCopy);
	// jchar * (JNICALL *GetCharArrayElements)
	// (JNIEnv *env, jcharArray array, jboolean *isCopy);
	// jshort * (JNICALL *GetShortArrayElements)
	// (JNIEnv *env, jshortArray array, jboolean *isCopy);
	// jint * (JNICALL *GetIntArrayElements)
	// (JNIEnv *env, jintArray array, jboolean *isCopy);
	// jlong * (JNICALL *GetLongArrayElements)
	// (JNIEnv *env, jlongArray array, jboolean *isCopy);
	// jfloat * (JNICALL *GetFloatArrayElements)
	// (JNIEnv *env, jfloatArray array, jboolean *isCopy);
	// jdouble * (JNICALL *GetDoubleArrayElements)
	// (JNIEnv *env, jdoubleArray array, jboolean *isCopy);

	// void (JNICALL *ReleaseBooleanArrayElements)
	// (JNIEnv *env, jbooleanArray array, jboolean *elems, jint mode);
	// void (JNICALL *ReleaseByteArrayElements)
	// (JNIEnv *env, jbyteArray array, jbyte *elems, jint mode);
	// void (JNICALL *ReleaseCharArrayElements)
	// (JNIEnv *env, jcharArray array, jchar *elems, jint mode);
	// void (JNICALL *ReleaseShortArrayElements)
	// (JNIEnv *env, jshortArray array, jshort *elems, jint mode);
	// void (JNICALL *ReleaseIntArrayElements)
	// (JNIEnv *env, jintArray array, jint *elems, jint mode);
	// void (JNICALL *ReleaseLongArrayElements)
	// (JNIEnv *env, jlongArray array, jlong *elems, jint mode);
	// void (JNICALL *ReleaseFloatArrayElements)
	// (JNIEnv *env, jfloatArray array, jfloat *elems, jint mode);
	// void (JNICALL *ReleaseDoubleArrayElements)
	// (JNIEnv *env, jdoubleArray array, jdouble *elems, jint mode);

	// void (JNICALL *GetBooleanArrayRegion)
	// (JNIEnv *env, jbooleanArray array, jsize start, jsize l, jboolean *buf);
	// void (JNICALL *GetByteArrayRegion)
	// (JNIEnv *env, jbyteArray array, jsize start, jsize len, jbyte *buf);
	// void (JNICALL *GetCharArrayRegion)
	// (JNIEnv *env, jcharArray array, jsize start, jsize len, jchar *buf);
	// void (JNICALL *GetShortArrayRegion)
	// (JNIEnv *env, jshortArray array, jsize start, jsize len, jshort *buf);
	// void (JNICALL *GetIntArrayRegion)
	// (JNIEnv *env, jintArray array, jsize start, jsize len, jint *buf);
	// void (JNICALL *GetLongArrayRegion)
	// (JNIEnv *env, jlongArray array, jsize start, jsize len, jlong *buf);
	// void (JNICALL *GetFloatArrayRegion)
	// (JNIEnv *env, jfloatArray array, jsize start, jsize len, jfloat *buf);
	// void (JNICALL *GetDoubleArrayRegion)
	// (JNIEnv *env, jdoubleArray array, jsize start, jsize len, jdouble *buf);

	// void (JNICALL *SetBooleanArrayRegion)
	// (JNIEnv *env, jbooleanArray array, jsize start, jsize l, jboolean *buf);
	// void (JNICALL *SetByteArrayRegion)
	// (JNIEnv *env, jbyteArray array, jsize start, jsize len, jbyte *buf);
	// void (JNICALL *SetCharArrayRegion)
	// (JNIEnv *env, jcharArray array, jsize start, jsize len, jchar *buf);
	// void (JNICALL *SetShortArrayRegion)
	// (JNIEnv *env, jshortArray array, jsize start, jsize len, jshort *buf);
	// void (JNICALL *SetIntArrayRegion)
	// (JNIEnv *env, jintArray array, jsize start, jsize len, jint *buf);
	// void (JNICALL *SetLongArrayRegion)
	// (JNIEnv *env, jlongArray array, jsize start, jsize len, jlong *buf);
	// void (JNICALL *SetFloatArrayRegion)
	// (JNIEnv *env, jfloatArray array, jsize start, jsize len, jfloat *buf);
	// void (JNICALL *SetDoubleArrayRegion)
	// (JNIEnv *env, jdoubleArray array, jsize start, jsize len, jdouble *buf);

	// jint (JNICALL *RegisterNatives)
	// (JNIEnv *env, jclass clazz, const JNINativeMethod *methods, 
	// jint nMethods);
	// jint (JNICALL *UnregisterNatives)
	// (JNIEnv *env, jclass clazz);

	// jint (JNICALL *MonitorEnter)
	// (JNIEnv *env, jobject obj);
	// jint (JNICALL *MonitorExit)
	// (JNIEnv *env, jobject obj);

	// jint (JNICALL *GetJavaVM)
	// (JNIEnv *env, JavaVM **vm);
};

nsHashtable* nsJNIEnv::theIDTable = NULL;

nsJNIEnv::nsJNIEnv(nsISecureJNI2* secureEnv)
	:	mSecureEnv(secureEnv), mJavaThread(NULL)
{
	this->functions = &theFuncs;
	if (theIDTable == NULL)
		theIDTable = new nsHashtable();
}

nsJNIEnv::~nsJNIEnv()
{
	this->functions = NULL;
}
