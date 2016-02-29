
#include "stdafx.h"
#include "vstsdk2.4/public.sdk/source/vst2.x/audioeffectx.h"

namespace {

class EmptyAudioEffect : public AudioEffectX {
public:
	typedef AudioEffectX BaseClass;

	EmptyAudioEffect(audioMasterCallback audioMaster)
		: BaseClass(audioMaster, 1, 0) {
		this->setNumInputs(2);
		this->setNumOutputs(2);
		this->isSynth(false);
		this->setUniqueID(*reinterpret_cast<const int*>("amml"));  //This code should be registered with Steinberg to ensure uniqueness.
		this->canDoubleReplacing(true);
	}
	virtual ~EmptyAudioEffect() {
	}

	virtual void processReplacing(float** inputs, float** outputs, VstInt32 sampleFrames) override {
	}
	virtual void processDoubleReplacing(double** inputs, double** outputs, VstInt32 sampleFrames) override {
	}

	virtual bool getEffectName(char* name) override {
		strcpy(name, "AwesomeMouseMode");
		return true;
	}
	virtual bool getVendorString(char* text) override {
		strcpy(text, "nano");
		return true;
	}
	virtual bool getProductString(char* text) override {
		strcpy(text, "AwesomeMouseMode");
		return true;
	}
	virtual VstInt32 getVendorVersion() override {
		return 0;
	}
	virtual VstInt32 canDo(char* text) override {
		return 0;
	}
};

} // namespace




extern "C" {

#if defined (__GNUC__) && ((__GNUC__ >= 4) || ((__GNUC__ == 3) && (__GNUC_MINOR__ >= 1)))
	#define VST_EXPORT	__attribute__ ((visibility ("default")))
#else
	#define VST_EXPORT
#endif

__declspec(dllexport) VST_EXPORT AEffect* __stdcall VSTMain(audioMasterCallback audioMaster) {
	// Get VST Version of the Host.
	if (!audioMaster(0, audioMasterVersion, 0, 0, 0, 0)) {
		return nullptr;  // old version
	}

	EmptyAudioEffect* effect = new EmptyAudioEffect(audioMaster);
	if (!effect) {
		return nullptr;
	}
	return effect->getAeffect();
}

#if (TARGET_API_MAC_CARBON && __ppc__)
VST_EXPORT AEffect* main_macho(audioMasterCallback audioMaster) { return VSTMain(audioMaster); }
#elif WIN32
VST_EXPORT AEffect* MAIN(audioMasterCallback audioMaster) { return VSTMain(audioMaster); }
#elif BEOS
VST_EXPORT AEffect* main_plugin(audioMasterCallback audioMaster) { return VSTMain(audioMaster); }
#endif

} // extern "C"
