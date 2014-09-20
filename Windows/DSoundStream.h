#ifndef __SOUNDSTREAM_H__
#define __SOUNDSTREAM_H__

#include "Common/CommonWindows.h"

namespace WinAudio {

	enum AudioSampleFormat {
		FMT_S16,
		FMT_FLOAT32,
	};

	struct AudioFormat {
		int sampleRateHz;
		int numChannels;
		AudioSampleFormat sampleFormat;
	};

	typedef int(*StreamCallback)(short *buffer, int numSamples, int bits, int rate, int channels);

	class AudioBackend {
	public:
		virtual ~AudioBackend() {}

		virtual bool StartSound(HWND window, StreamCallback _callback) = 0;
		virtual void UpdateSound() {}
		virtual void StopSound() = 0;

		virtual void GetAudioFormat(AudioFormat *fmt) = 0;
	};

	class DSound : public AudioBackend {
	public:
		bool StartSound(HWND window, StreamCallback _callback);
		void UpdateSound();
		void StopSound();

		virtual void GetAudioFormat(AudioFormat *fmt);

	private:
		void soundThread();
		static unsigned int WINAPI soundThreadTrampoline(void *);

		StreamCallback callback;
	};

}  // namespace

 
#endif //__SOUNDSTREAM_H__