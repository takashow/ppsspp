#include "native/thread/threadutil.h"
#include "Common/CommonWindows.h"
#include <dsound.h>
#include <XAudio2.h>

#include "dsoundstream.h"	

namespace WinAudio
{
	bool DSound::createBuffer(const AudioFormat &format) {
		PCMWAVEFORMAT pcmwf; 
		DSBUFFERDESC dsbdesc; 

		// No support for float audio here.
		if (format.sampleFormat != FMT_S16) {
			return false;
		}
		format_ = format;

		memset(&pcmwf, 0, sizeof(PCMWAVEFORMAT)); 
		memset(&dsbdesc, 0, sizeof(DSBUFFERDESC)); 

		pcmwf.wf.wFormatTag = WAVE_FORMAT_PCM; 
		pcmwf.wf.nChannels = format.numChannels;
		pcmwf.wf.nSamplesPerSec = format.sampleRateHz;
		pcmwf.wf.nBlockAlign = 4; 
		pcmwf.wf.nAvgBytesPerSec = pcmwf.wf.nSamplesPerSec * pcmwf.wf.nBlockAlign; 
		pcmwf.wBitsPerSample = 16;

		dsbdesc.dwSize = sizeof(DSBUFFERDESC); 
		dsbdesc.dwFlags = DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_GLOBALFOCUS; // //DSBCAPS_CTRLPAN | DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLFREQUENCY; 
		dsbdesc.dwBufferBytes = bufferSize = BUFSIZE;  //FIX32(pcmwf.wf.nAvgBytesPerSec);   //change to set buffer size
		dsbdesc.lpwfxFormat = (WAVEFORMATEX *)&pcmwf; 

		if (SUCCEEDED(ds->CreateSoundBuffer(&dsbdesc, &dsBuffer, NULL))) { 
			dsBuffer->SetCurrentPosition(0);
			return true; 
		} else { 
			dsBuffer = NULL; 
			return false; 
		} 
	}

	bool DSound::writeDataToBuffer(DWORD dwOffset, char* soundData, DWORD dwSoundBytes) {
		void *ptr1, *ptr2;
		DWORD numBytes1, numBytes2; 
		// Obtain memory address of write block. This will be in two parts if the block wraps around.
		HRESULT hr = dsBuffer->Lock(dwOffset, dwSoundBytes, &ptr1, &numBytes1, &ptr2, &numBytes2, 0);
		if (SUCCEEDED(hr)) { 
			memcpy(ptr1, soundData, numBytes1); 
			if (ptr2!=0) 
				memcpy(ptr2, soundData+numBytes1, numBytes2); 
			dsBuffer->Unlock(ptr1, numBytes1, ptr2, numBytes2); 
			return true; 
		}
		return false; 
	} 

	void DSound::soundThread() {
		setCurrentThreadName("DSound");
		currentPos = 0;
		lastPos = 0;

		dsBuffer->Play(0, 0, DSBPLAY_LOOPING);

		while (!threadData) {
			EnterCriticalSection(&soundCriticalSection);

			dsBuffer->GetCurrentPosition((DWORD *)&currentPos, 0);
			int numBytesToRender = RoundDown128(ModBufferSize(currentPos - lastPos));
			if (numBytesToRender >= 256) {
				int numBytesRendered = 4 * (*callback)(realtimeBuffer, numBytesToRender >> 2, 16, format_.sampleRateHz, format_.numChannels);
				//We need to copy the full buffer, regardless of what the mixer claims to have filled
				//If we don't do this then the sound will loop if the sound stops and the mixer writes only zeroes
				numBytesRendered = numBytesToRender;
				writeDataToBuffer(lastPos, (char *)realtimeBuffer, numBytesRendered);

				currentPos = ModBufferSize(lastPos + numBytesRendered);
				totalRenderedBytes += numBytesRendered;

				lastPos = currentPos;
			}

			LeaveCriticalSection(&soundCriticalSection);
			WaitForSingleObject(soundSyncEvent, MAXWAIT);
		}
		dsBuffer->Stop();

		threadData = 2;
	}

	unsigned int WINAPI DSound::soundThreadTrampoline(void *userdata) {
		DSound *dsound = (DSound *)userdata;
		dsound->soundThread();
		return 0;
	}

	bool DSound::StartSound(HWND window, const AudioFormat &format, StreamCallback _callback) {
		callback = _callback;
		threadData=0;

		soundSyncEvent=CreateEvent(0,false,false,0);  
		InitializeCriticalSection(&soundCriticalSection);

		if (FAILED(DirectSoundCreate8(0,&ds,0)))
			return false;

		ds->SetCooperativeLevel(window,DSSCL_PRIORITY);
		if (!createBuffer(format))
			return false;

		DWORD num1;
		short *p1; 

		dsBuffer->Lock(0, bufferSize, (void **)&p1, &num1, 0, 0, 0); 

		memset(p1,0,num1);
		dsBuffer->Unlock(p1,num1,0,0);
		totalRenderedBytes = -bufferSize;
		hThread = (HANDLE)_beginthreadex(0, 0, soundThreadTrampoline, (void *)this, 0, 0);
		SetThreadPriority(hThread, THREAD_PRIORITY_ABOVE_NORMAL);
		return true;
	}

	void DSound::UpdateSound() {
		if (soundSyncEvent != NULL)
			SetEvent(soundSyncEvent);
	}

	void DSound::StopSound() {
		if (!dsBuffer)
			return;

		EnterCriticalSection(&soundCriticalSection);

		if (threadData == 0)
			threadData = 1;

		if (hThread != NULL) {
			WaitForSingleObject(hThread, 1000);
			CloseHandle(hThread);
			hThread = NULL;
		}

		if (threadData == 2) {
			if (dsBuffer != NULL)
				dsBuffer->Release();
			dsBuffer = NULL;
			if (ds != NULL)
				ds->Release();
			ds = NULL;
		}

		if (soundSyncEvent != NULL)
			CloseHandle(soundSyncEvent);
		soundSyncEvent = NULL;
		LeaveCriticalSection(&soundCriticalSection);
	}

	typedef HRESULT(__stdcall *XAudio2CreateFunc)(IXAudio2** ppXAudio2, UINT32 Flags, XAUDIO2_PROCESSOR XAudio2Processor);

	struct XA2State {
		IXAudio2 *xaudio2_;
		IXAudio2MasteringVoice *masteringVoice_;
		IXAudio2SourceVoice *sourceVoice_;
	};

	XAudio2::XAudio2() {
		// TODO: Tweak the buffers.
		audioBufferSize_ = 1024;
		bufferCount_ = 4;
		currentBuffer_ = 0;
		buffers_ = new short*[bufferCount_];
		xa2_ = new XA2State();
		memset(xa2_, 0, sizeof(*xa2_));
		for (int i = 0; i < bufferCount_; i++) {
			buffers_[i] = new short[audioBufferSize_];
		}
		CoInitializeEx(NULL, COINIT_MULTITHREADED);

		// Grab the pointer to the XAudio2Create function.
		library_ = LoadLibrary(L"xaudio2_8.dll");
		if (library_) {
			create_ = GetProcAddress(library_, "XAudio2Create");
		} else {
			create_ = NULL;
		}
	}

	XAudio2::~XAudio2() {
		delete xa2_;
		for (int i = 0; i < bufferCount_; i++) {
			delete[] buffers_[i];
		}
		delete[] buffers_;
		FreeLibrary(library_);
		CoUninitialize();
	}

	bool XAudio2::StartSound(HWND window, const AudioFormat &format, StreamCallback callback) {
		if (!create_)
			return false;

		callback_ = callback;
		format_ = format;
		started_ = false;

		XAudio2CreateFunc func = (XAudio2CreateFunc)create_;

		// For some reason, when loading the func dynamically, voices don't get created later (but still return S_OK?)
		/// HRESULT result = func(&xa2_->xaudio2_, 0, XAUDIO2_DEFAULT_PROCESSOR);

		// This states that loading dynamically should work:
		// http://blogs.msdn.com/b/chuckw/archive/2012/04/02/xaudio2-and-windows-8-consumer-preview.aspx

		HRESULT result = XAudio2Create(&xa2_->xaudio2_, 0, XAUDIO2_DEFAULT_PROCESSOR);
		if (FAILED(result)) {
			return false;
		}

		result = xa2_->xaudio2_->CreateMasteringVoice(&xa2_->masteringVoice_);
		if (FAILED(result)) {
			xa2_->xaudio2_->Release();
			return false;
		}

		WAVEFORMATEX fmt;
		fmt.cbSize = sizeof(fmt);
		fmt.nChannels = format.numChannels;
		fmt.nSamplesPerSec = format.sampleRateHz;
		fmt.wBitsPerSample = 16;
		fmt.nBlockAlign = sizeof(short) * format.numChannels;
		fmt.wFormatTag = 1;
		fmt.nAvgBytesPerSec = fmt.nBlockAlign * fmt.nSamplesPerSec;
		result = xa2_->xaudio2_->CreateSourceVoice(&xa2_->sourceVoice_, &fmt, 0, 2.0f, 0, 0, 0);
		if (FAILED(result)) {
			xa2_->masteringVoice_->DestroyVoice();
			xa2_->masteringVoice_ = nullptr;
			xa2_->xaudio2_->Release();
			xa2_->xaudio2_ = nullptr;
			return false;
		}

		return true;
	}

	// TODO: Try pushing buffers of whatever size the PSP gives us directly from the PSP APIs instead of pulling. Would let us
	// get rid of one audio queue, now we really have two...
	void XAudio2::UpdateSound() {
		// Find the current state of the playing buffers
		XAUDIO2_VOICE_STATE state;
		xa2_->sourceVoice_->GetState(&state);

		// Have any of the buffer completed
		while (state.BuffersQueued < bufferCount_) {
			// Generate a new buffer of data.
			callback_(buffers_[currentBuffer_], audioBufferSize_ / 2, 16, format_.sampleRateHz, format_.numChannels);

			// Submit the new buffer
			XAUDIO2_BUFFER buf = { 0 };
			buf.AudioBytes = sizeof(short) * audioBufferSize_;
			buf.pAudioData = (BYTE *)buffers_[currentBuffer_];
			HRESULT result = xa2_->sourceVoice_->SubmitSourceBuffer(&buf);

			if (!started_) {
				xa2_->sourceVoice_->Start(0, XAUDIO2_COMMIT_NOW);
				started_ = true;
			}

			// Advance the buffer index
			currentBuffer_ = ++currentBuffer_ % bufferCount_;

			// Get the updated state
			xa2_->sourceVoice_->GetState(&state);
		}
	}

	void XAudio2::StopSound() {
		// Submit a final buffer to avoid counting a glitch.
		XAUDIO2_BUFFER buf = { 0 };
		buf.AudioBytes = NULL;
		buf.Flags = XAUDIO2_END_OF_STREAM;
		xa2_->sourceVoice_->SubmitSourceBuffer(&buf);
		xa2_->sourceVoice_->DestroyVoice();
		xa2_->masteringVoice_->DestroyVoice();
		xa2_->xaudio2_->Release();
		memset(xa2_, 0, sizeof(*xa2_));
		started_ = false;
	}

}
