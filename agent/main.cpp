// Compiled with g++ -fPIC -I$JAVA_HOME/include -shared -o libagent.so main.cpp

// TODO: Add doxygen comments

#include <jvmti.h>
#include <string.h>

void JNICALL VMInit(jvmtiEnv *jvmti_env, JNIEnv* jni_env, jthread thread);
void check_jvmti_error(jvmtiEnv *jvmti, jvmtiError errnum, const char *str);
jvmtiError GetClassBySignature(jvmtiEnv* jvmti, const char* signature, jclass* klass);
jvmtiError GetFieldIDByName(jvmtiEnv* jvmti, jclass klass, const char* name, jfieldID* fieldID);
void JNICALL FieldModification(jvmtiEnv *jvmti_env, JNIEnv* jni_env,
                jthread thread, jmethodID method, jlocation location,
                jclass field_klass, jobject object, jfieldID field,
                char signature_type, jvalue new_value);

JNIEXPORT jint JNICALL Agent_OnLoad(JavaVM* jvm, char* options, void* reserved) {
	static jvmtiEnv* jvmti = NULL;
	static jvmtiCapabilities capabilities;
	jvmtiError error;
	jvmtiEventCallbacks callbacks;
	
	memset(&capabilities, 0, sizeof(capabilities));
	memset(&callbacks, 0, sizeof(callbacks));

	// Get JVMTI environment
	jint env_error = jvm->GetEnv((void **)&jvmti, JVMTI_VERSION_1_0);
	if (env_error != JNI_OK || jvmti == NULL) {
		printf("Failed to get JVMTI environment.");
		return env_error;
	}

	// Enable capability to get source code line numbers and source file
	capabilities.can_get_line_numbers = 1;
	capabilities.can_get_source_file_name = 1;

	// Enable capability to receive events for field modifications and the event itself                
	capabilities.can_generate_field_modification_events = 1;

	error = jvmti->AddCapabilities(&capabilities);
	check_jvmti_error(jvmti, error, "Unable to get necessary JVMTI capabilities.");

	error = jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_FIELD_MODIFICATION, NULL);
    check_jvmti_error(jvmti, error, "Unable to set JVMTI_EVENT_FIELD_MODIFICATION.");

	// Enable VMInit event so that we know when the JVM is initialized and we 
	// can finish the rest of the setup
	error = jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_VM_INIT, NULL);
    check_jvmti_error(jvmti, error, "Unable to set JVMTI_EVENT_VM_INIT.");

	callbacks.VMInit = &VMInit;

	// Set a callback to receive events when the security field of System is set.
	// This will let us see when the security manager is being changed.
	callbacks.FieldModification = &FieldModification;

	error = jvmti->SetEventCallbacks(&callbacks, (jint)sizeof(callbacks));
	check_jvmti_error(jvmti, error, "Unable to register callback for field modification events.");

	return JNI_OK;
}

void JNICALL VMInit(jvmtiEnv *jvmti, JNIEnv* jni_env, jthread thread) {
	jclass system_class;
	jfieldID securityID; 
	jvmtiError error;

	// Get the security field of the System class (holds the SecurityManager) and
	// set a modification watch on it
	error = GetClassBySignature(jvmti, "Ljava/lang/System;", &system_class);
	check_jvmti_error(jvmti, error, "Unable to get System class.");

	error = GetFieldIDByName(jvmti, system_class, "security", &securityID);
	check_jvmti_error(jvmti, error, "Unable to get security field of the System class.");

	error = jvmti->SetFieldModificationWatch(system_class, securityID);
	check_jvmti_error(jvmti, error, "Unable to set a watch on modifications of security field of System class.");
}

void print_jvmti_error(jvmtiEnv* jvmti, jvmtiError errnum, const char* str)
{
    char* errnum_str = NULL;

    jvmti->GetErrorName(errnum, &errnum_str);
    printf("ERROR: JVMTI: %d(%s): %s\n", errnum, errnum_str == NULL ? "Unknown" : errnum_str, 
		str == NULL ? "" : str);
}

void check_jvmti_error(jvmtiEnv* jvmti, jvmtiError errnum, const char* str)
{
    if (errnum != JVMTI_ERROR_NONE)
        print_jvmti_error(jvmti, errnum, str);
}

jvmtiError GetClassBySignature(jvmtiEnv* jvmti, const char* signature, jclass* klass) {
	jint class_count = 0;
	jclass* classes = NULL;
	jvmtiError error;

	error = jvmti->GetLoadedClasses(&class_count, &classes);
	if (error != JVMTI_ERROR_NONE)
		return error;


	for (size_t i = 0; i < class_count; i++) {
		char* class_signature = NULL;

		error = jvmti->GetClassSignature(classes[i], &class_signature, NULL);
		if (error != JVMTI_ERROR_NONE)
			return error;

		if (strcmp(class_signature, signature) == 0) {
			*klass = classes[i];
			break;
		}
	}

	return JVMTI_ERROR_NONE;
}

jvmtiError GetFieldIDByName(jvmtiEnv* jvmti, jclass klass, const char* name, jfieldID* fieldID) {
	jint field_count = 0;
	jfieldID* fields = NULL;
	jvmtiError error;

	error = jvmti->GetClassFields(klass, &field_count, &fields);
	if (error != JVMTI_ERROR_NONE)
		return error;

	for (size_t i = 0; i < field_count; i++) {
		char* field_name = NULL;

		error = jvmti->GetFieldName(klass, fields[i], &field_name, NULL, NULL);
		if (error != JVMTI_ERROR_NONE)
			return error;

		if (strcmp(field_name, name) == 0) {
			*fieldID = fields[i];
			break;
		}
	}

	return JVMTI_ERROR_NONE;
}

void JNICALL FieldModification(jvmtiEnv* jvmti, JNIEnv* jni_env,
                jthread thread, jmethodID method, jlocation location,
                jclass field_klass, jobject object, jfieldID field,
                char signature_type, jvalue new_value) {

	jvmtiError error;
	jvmtiFrameInfo caller_frame;
	jint frame_count = 0;
	jint line_count = 0;
	jvmtiLineNumberEntry* line_table = NULL;
	jint line_number = 0;
	char* method_name = NULL;
	jclass caller_class = NULL;
	char* source_file_name = NULL;

	// Get caller (we have to do this because we are in setSecurityManager when
	// this method is called);
	error = jvmti->GetStackTrace(thread, 2, 1, &caller_frame, &frame_count);
	check_jvmti_error(jvmti, error, "Unable to get stack frame.");

	// Get caller's line number	
	error = jvmti->GetLineNumberTable(caller_frame.method, &line_count, &line_table);	
	check_jvmti_error(jvmti, error, "Unable to get line number for SecurityManager change.");

	for (size_t i = 0; i < line_count; i++) {
		if (line_table[i].start_location > caller_frame.location)
			break;

		line_number = line_table[i].line_number;
	}

	// Get caller's method name
	error = jvmti->GetMethodName(caller_frame.method, &method_name, NULL, NULL);
	check_jvmti_error(jvmti, error, "Unable to method name.");

	// Get the caller's class and source file name
	error = jvmti->GetMethodDeclaringClass(caller_frame.method, &caller_class);
	check_jvmti_error(jvmti, error, "Unable to get caller class.");

	error = jvmti->GetSourceFileName(caller_class, &source_file_name);

	// If new_value is full a null SecurityManager raise a red flag
	if ((long)new_value.j == 0) {
		printf("WARNING: The SecurityManager is being disabled!!!\n");
	} else {
		jclass new_manager = jni_env->GetObjectClass(new_value.l);

		// TODO: Query the fields of the class to see if we have weakened the manager
	}

	printf("SecurityManager Changed:\n%s, %s, %d\n\n", source_file_name, method_name, line_number);

}
