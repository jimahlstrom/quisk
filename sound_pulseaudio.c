/*
 * sound_pulseaudio.c is part of Quisk, and is Copyright the following
 * authors:
 * 
 * Philip G. Lee <rocketman768@gmail.com>, 2014
 * Jim Ahlstrom, N2ADR, October, 2014
 * Eric Thornton, KM4DSJ, September, 2015
 * 
 * This code replaces the pulseaudio-simple version by Philip G. Lee.  It
 * uses the asynchronous pulseaudio API.  It was written by Eric Thornton, 2015.
 *
 * Quisk is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.

 * Quisk is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */


/* This pulseaudio interface was rewritten to utilize the features in the 
 * asynchronous API and threaded mainloop.
 * Eric Thornton, KM4DSJ 2015
*/
/*
 * 2017 Nov by N2ADR Remove most exit(1) statements.
 * 2017 Nov by N2ADR Add code to enable PulseAudio to continue with mis-spelled device names.
 * 2020 Apr by N2ADR Changes in printf's
*/

#ifdef QUISK_HAVE_PULSEAUDIO

#include <Python.h>
#include <stdio.h>
#include <string.h>
#include <complex.h>
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include "quisk.h"
#include <pulse/pulseaudio.h>

// From pulsecore/macro.h
#define pa_memzero(x,l) (memset((x), 0, (l)))
#define pa_zero(x) (pa_memzero(&(x), sizeof(x)))

// Current sound status
extern struct sound_conf quisk_sound_state;

//pointers for aychronous threaded loop
static pa_threaded_mainloop *pa_ml;
static pa_mainloop_api *pa_mlapi;
static pa_context *pa_ctx; 		//local context
static pa_context *pa_IQ_ctx; 		//remote context for IQ audio
volatile int streams_ready = 0;		//This is ++/-- by the mainloop thread
volatile int streams_to_start;

// remember all open devices for easy cleanup on exit
static pa_stream *OpenPulseDevices[PA_LIST_SIZE * 2] = {NULL};

static int have_QuiskDigitalInput;	// Do we need to create this null sink?
static int have_QuiskDigitalOutput;	// Do we need to create this null sink?

static int underrunPlayback;
static int tlength_bytes;

/* This callback happens any time a stream changes state. Here, it's primary used to 
 * tell the quisk thread when streams are ready. 
 */

static void stream_state_callback(pa_stream *s, void *userdata) {
    struct sound_dev *dev = userdata;
    unsigned int S;
    assert(s);
    assert(dev);
    
    dev->pulse_stream_state = pa_stream_get_state(s);
    switch (dev->pulse_stream_state) {
        case PA_STREAM_CREATING:
	    if (quisk_sound_state.verbose_pulse)
		printf("\n**Stream state Creating: %s; %s\n", dev->stream_description, dev->name);
            break;
            
        case PA_STREAM_TERMINATED:
	    if (quisk_sound_state.verbose_pulse)
		printf("\n**Stream state Terminated: %s; %s\n", dev->stream_description, dev->name);
            streams_ready--;
            break;
            
        case PA_STREAM_READY:
	    if (quisk_sound_state.verbose_pulse)
		printf("\n**Stream state Ready: %s; %s\n", dev->stream_description, dev->name);
            streams_ready++; //increment counter to tell other thread that this stream is ready
	    streams_to_start++;
            if (quisk_sound_state.verbose_pulse) {
                const pa_buffer_attr *a;
                printf("   Connected to device index %u, %ssuspended: %s.\n",
			pa_stream_get_device_index(s),
                       pa_stream_is_suspended(s) ? "" : "not ",
                       pa_stream_get_device_name(s));

		S = dev->num_channels * dev->sample_bytes * dev->sample_rate / 1000;
                if (!(a = pa_stream_get_buffer_attr(s)))
                    printf("   pa_stream_get_buffer_attr() failed: %s", pa_strerror(pa_context_errno(pa_stream_get_context(s))));
                else if (!(a->prebuf)) {
                    printf("   Buffer metrics msec: rate %d, channels %d, sample bytes %d, maxlength=%u, fragsize=%u\n",
			dev->sample_rate, dev->num_channels, dev->sample_bytes, a->maxlength/S, a->fragsize/S);
                }
                else {
                    printf("   Buffer metrics msec: rate %d, channels %d, sample bytes %d, maxlength=%u, prebuf=%u, tlength=%u  minreq=%u\n",
			dev->sample_rate, dev->num_channels, dev->sample_bytes, a->maxlength/S, a->prebuf/S, a->tlength/S, a->minreq/S);
                }
            }
            break;
        
        case PA_STREAM_FAILED:
        default:
	    snprintf(dev->dev_errmsg, QUISK_SC_SIZE,
		"%.60s: %.60s", dev->device_name, pa_strerror(pa_context_errno(pa_stream_get_context(s))));
	    snprintf(quisk_sound_state.err_msg, QUISK_SC_SIZE,
		"Stream error: %.40s - %.40s", dev->name, pa_strerror(pa_context_errno(pa_stream_get_context(s))));
	    if (quisk_sound_state.verbose_pulse)
		printf("\n**Stream state Failed: %s; %s\n", dev->stream_description, dev->device_name);
            printf("%s\n", quisk_sound_state.err_msg);
	    streams_to_start++;
            break;
    }
}


//Indicates underflow on passed stream (playback)
static void stream_underflow_callback(pa_stream *s, void *userdata) {
    struct sound_dev *dev = userdata;
    assert(s);
    assert(dev);
    
    if (quisk_sound_state.verbose_pulse)
        printf("Stream underrun %s\n", dev->stream_description);
    dev->dev_error++;
    if (dev->dev_index == t_Playback)
        underrunPlayback = 1;
}

//Indicates overflow on passed stream (record)
static void stream_overflow_callback(pa_stream *s, void *userdata) {
    struct sound_dev *dev = userdata;
    assert(s);
    
    if (quisk_sound_state.verbose_pulse)
        printf("Stream overrun %s\n", dev->stream_description);
    dev->dev_error++;
}

//Indicates stream has started
static void stream_started_callback(pa_stream *s, void *userdata) {
    struct sound_dev *dev = userdata;
    assert(s);
    assert(dev);
    
    if (quisk_sound_state.verbose_pulse)
        printf("Stream started %s\n", dev->stream_description);
}

//Called when cork/uncork has succeeded on passed stream. Signals mainloop when complete.
static void stream_corked_callback(pa_stream *s, int success, void *userdata) {
    assert(s);
    struct sound_dev *dev = userdata;
    
    if (success) {
        if (quisk_sound_state.verbose_pulse)
            printf("Stream cork/uncork %s success\n", dev->stream_description);
        pa_threaded_mainloop_signal(pa_ml, 0);
    }
    else {
        if (quisk_sound_state.verbose_pulse)
            printf("Stream cork/uncork %s Failure!\n", dev->stream_description);
        exit(11);
    }
}

// Called when stream flush has completed.
static void stream_flushed_callback(pa_stream *s, int success, void *userdata) {
    assert(s);
    struct sound_dev *dev = userdata;
    
    if (success) {
        printf("Stream flush %s success\n", dev->stream_description);
        pa_threaded_mainloop_signal(pa_ml, 0);
    }
    else {
        printf("Stream flush %s Failure!\n", dev->stream_description);
        exit(12);
    }
    pa_threaded_mainloop_signal(pa_ml, 0);
}

// This is called by the play function when the timing structure is updated.
static void stream_timing_callback(pa_stream *s, int success, void *userdata) {
    struct sound_dev *dev = userdata;
    pa_usec_t l;
    int negative = 0;
    assert(s);

    if (!success || pa_stream_get_latency(s, &l, &negative) < 0) {
        printf("pa_stream_get_latency() failed: %s\n", pa_strerror(pa_context_errno(pa_stream_get_context(s))));
        return;
    }

    dev->dev_latency = (int)l;
    if (negative)
        dev->dev_latency *= -1;
    pa_threaded_mainloop_signal(pa_ml, 0);
}

// This is used to change the radio sound playback buffer attributes so the sidetone is faster
static void buffer_attr_callback(pa_stream * s, int success, void * userdata) {
    struct sound_dev * dev = userdata;
    assert(s);

    if (success) {
        printf("Change buffer attr %s success\n", dev->stream_description);
        pa_threaded_mainloop_signal(pa_ml, 0);
    }
    else {
        printf("Change buffer attr %s Failure!\n", dev->stream_description);
        exit(17);
    }
}
void quisk_set_play_attr(struct sound_dev * dev, const pa_buffer_attr * attr) {
    pa_stream *s = dev->handle;
    pa_operation *o;
    
    pa_threaded_mainloop_lock(pa_ml);
    
    if (!(o = pa_stream_set_buffer_attr	(s, attr, buffer_attr_callback, dev))) {
        printf("quisk_play_attr(): %s\n", pa_strerror(pa_context_errno(pa_stream_get_context(s))));
        exit(14);
    }
    else {
        while(pa_operation_get_state(o) == PA_OPERATION_RUNNING)
            pa_threaded_mainloop_wait(pa_ml);
        pa_operation_unref(o);
    }
    pa_threaded_mainloop_unlock(pa_ml);
}


/* This callback allows us to read in the server side stream information so that we
 * can match stream formats and sizes to what is configured in pulseaudio.
*/

static void server_info_cb(pa_context *c, const pa_server_info *info, void *userdata) {
    int bytes_per_frame;
    struct sound_dev **pDevices = userdata;
    pa_buffer_attr rec_ba;
    pa_buffer_attr play_ba;
    pa_sample_spec ss;
    pa_sample_spec default_ss;
    pa_stream_flags_t pb_flags;
    pa_stream_flags_t rec_flags = PA_STREAM_ADJUST_LATENCY;
    default_ss = info->sample_spec;

    printf("Connected to %s \n", info->host_name);

    while(*pDevices) {
        struct sound_dev *dev = *pDevices++;
        const char *dev_name;
        pa_stream *s;
        
        pa_zero(rec_ba);
        pa_zero(play_ba);

	dev_name = dev->device_name;
	if (strcmp(dev_name, "default") == 0)		// the device name is "default" for the default device
		dev_name = NULL;

        if (quisk_sound_state.verbose_pulse)
            printf("Opening Device %s", dev_name);

        //Construct sample specification. Use S16LE if availiable. Default to Float32 for others.
        //If the source/sink is not Float32, Pulseaudio will convert it (uses CPU)
        //dev->sample_bytes = (int)pa_frame_size(&ss) / ss.channels;
        if (default_ss.format == PA_SAMPLE_S16LE) {
            dev->sample_bytes = 2;
            ss.format = default_ss.format;
            dev->sound_format = Int16;
            if (quisk_sound_state.verbose_pulse)
                printf(" with sound format Int16\n");
        }
        else {
            dev->sample_bytes = 4;
            ss.format = PA_SAMPLE_FLOAT32LE;
            dev->sound_format = Float32;
            if (quisk_sound_state.verbose_pulse)
                printf(" with sound format Float32\n");
        }
        
        ss.rate = dev->sample_rate;
        ss.channels = dev->num_channels;
        bytes_per_frame = dev->sample_bytes * dev->num_channels;

        rec_ba.maxlength = (uint32_t) -1;
        rec_ba.fragsize = (uint32_t) SAMP_BUFFER_SIZE / 16;  //higher numbers eat cpu on reading monitor streams.
        play_ba.maxlength = (uint32_t) -1;
        if (dev->dev_index == t_Playback) {	// radio sound playback uses cork/uncork instead of prebuf
       	    play_ba.prebuf = 0;
            tlength_bytes = dev->sample_rate / 1000 * quisk_sound_state.data_poll_usec * bytes_per_frame * 2 / 1000;
            play_ba.tlength = tlength_bytes;
        }
        else {
       	    play_ba.prebuf = bytes_per_frame * dev->latency_frames;
       	    play_ba.tlength = play_ba.prebuf;
        }


        if (dev->latency_frames == 0)
            play_ba.minreq = (uint32_t) 0; //verify this is sane
        else
            play_ba.minreq = (uint32_t) -1;
        if (dev->stream_dir_record) {

            if (!(s = pa_stream_new(c, dev->stream_description, &ss, NULL))) {
                printf("pa_stream_new() failed: %s", pa_strerror(pa_context_errno(c)));
                continue;
            }
            if (pa_stream_connect_record(s, dev_name, &rec_ba, rec_flags) < 0) {
                printf("pa_stream_connect_record() failed: %s", pa_strerror(pa_context_errno(c)));
                continue;
            }
            pa_stream_set_overflow_callback(s, stream_overflow_callback, dev);
        }

        else {
            pa_cvolume cv;
            pa_volume_t volume = PA_VOLUME_NORM;

            if (!(s = pa_stream_new(c, dev->stream_description, &ss, NULL))) {
                printf("pa_stream_new() failed: %s", pa_strerror(pa_context_errno(c)));
                continue;
            }
            if (dev->dev_index == t_Playback) {
                pb_flags = PA_STREAM_START_CORKED;
                dev->cork_status = 1;
            }
            else {
                pb_flags = PA_STREAM_NOFLAGS;
            }
            if (pa_stream_connect_playback(s, dev_name, &play_ba, pb_flags, pa_cvolume_set(&cv, ss.channels, volume), NULL) < 0) {
                printf("pa_stream_connect_playback() failed: %s", pa_strerror(pa_context_errno(c)));
                continue;
            }
            pa_stream_set_underflow_callback(s, stream_underflow_callback, dev);
        }
        
        
        pa_stream_set_state_callback(s, stream_state_callback, dev);
        pa_stream_set_started_callback(s, stream_started_callback, dev);

        dev->handle = (void*)s; //save memory address for stream in handle

        int i;
        for(i=0;i < PA_LIST_SIZE;i++) {  //save address for stream for easy exit
            if (!(OpenPulseDevices[i])) {
                OpenPulseDevices[i] = dev->handle;
                break;
            }
        }
    }
}


//Context state callback. Basically here to pass initialization to server_info_cb
static void state_cb(pa_context *c, void *userdata) {
    pa_context_state_t state;
    state = pa_context_get_state(c);
    switch  (state) {
        // There are just here for reference
        case PA_CONTEXT_UNCONNECTED:
        case PA_CONTEXT_CONNECTING:
        case PA_CONTEXT_AUTHORIZING:
        case PA_CONTEXT_SETTING_NAME:
        default:
            break;
        case PA_CONTEXT_FAILED:
        case PA_CONTEXT_TERMINATED:
            printf("Context Terminated\n");
            break;
        case PA_CONTEXT_READY: {
            pa_operation *o;
            if (!(o = pa_context_get_server_info(c, server_info_cb, userdata)))
                printf("pa_context_get_server_info() failed: %s", pa_strerror(pa_context_errno(c)));
            else
                pa_operation_unref(o);
        }
    }
}


#if 0
/* Stream draining complete */
static void stream_drain_complete(pa_stream*s, int success, void *userdata) {
    struct sound_dev *dev = userdata;

    if (!success) {
        printf("Failed to drain stream: %s\n", pa_strerror(pa_context_errno(pa_stream_get_context(s))));
        quit(1);
    }

    if (quisk_sound_state.verbose_pulse)
        printf("Playback stream %s drained.\n", dev->name);
}


/*drain stream function*/
void quisk_drain_cork_stream(struct sound_dev *dev) {
    pa_stream *s = dev->handle;
    pa_operation *o;
   
    if (!(o = pa_stream_drain(s, stream_drain_complete, NULL))) {
        printf("pa_stream_drain(): %s\n", pa_strerror(pa_context_errno(pa_stream_get_context(s))));
        exit(1);
    }
    
    pa_operation_unref(o);
}

#endif

//Cork function. Holds mainloop lock until operation is completed.
void quisk_cork_pulseaudio(struct sound_dev *dev, int b) {
    pa_stream *s = dev->handle;
    pa_operation *o;
    
    pa_threaded_mainloop_lock(pa_ml);
    
    if (!(o = pa_stream_cork(s, b, stream_corked_callback, dev))) {
        printf("pa_stream_cork(): %s\n", pa_strerror(pa_context_errno(pa_stream_get_context(s))));
        exit(13);
    }
    else {
        while(pa_operation_get_state(o) == PA_OPERATION_RUNNING)
            pa_threaded_mainloop_wait(pa_ml);
        
        pa_operation_unref(o);
    }
    pa_threaded_mainloop_unlock(pa_ml);

    if (b)
        dev->cork_status = 1;
    else
        dev->cork_status = 0;
}

//Flush function. Holds mainloop lock until operation is completed.
void quisk_flush_pulseaudio(struct sound_dev *dev) {
    pa_stream *s = dev->handle;
    pa_operation *o;
    
    pa_threaded_mainloop_lock(pa_ml);
    
    if (!(o = pa_stream_flush(s, stream_flushed_callback, dev))) {
        printf("pa_stream_flush(): %s\n", pa_strerror(pa_context_errno(pa_stream_get_context(s))));
        exit(14);
    }
    else {
        while(pa_operation_get_state(o) == PA_OPERATION_RUNNING)
            pa_threaded_mainloop_wait(pa_ml);
        pa_operation_unref(o);
    }
    pa_threaded_mainloop_unlock(pa_ml);
}

static void WaitForPoll(void) {		// Implement a blocking read
    static double time0 = 0;		// start time in seconds
    double timer;			// time remaining from last poll usec

    timer = quisk_sound_state.data_poll_usec - (QuiskTimeSec() - time0) * 1e6;

    if (timer > 1000.0)     // see if enough time has elapsed
        QuiskSleepMicrosec((int)timer);		// wait for the remainder of the poll interval

    time0 = QuiskTimeSec();	// reset starting time value
}


/* Read samples from the PulseAudio device.
 * Samples are converted to complex form based upon format, counted
 * and returned via cSamples pointer.
 * Returns the number of samples placed into cSamples
 */
int quisk_read_pulseaudio(struct sound_dev *dev, complex double *cSamples)
{ // cSamples can be NULL to discard samples
    int i, nSamples;
    int read_frames;		// A frame is a sample from each channel
    const void * fbuffer;
    pa_stream *s = dev->handle;
    size_t read_bytes;
    
    if (!dev || dev->pulse_stream_state != PA_STREAM_READY)
        return 0;

    if (dev->cork_status) {
        if (dev->read_frames != 0) {
            WaitForPoll();
        }
        return 0;
    }

   // Note: Access to PulseAudio data from our sound thread requires locking the threaded mainloop.
    if (dev->read_frames == 0) {		// non-blocking: read available frames
        pa_threaded_mainloop_lock(pa_ml);
        read_frames = pa_stream_readable_size(s) / dev->num_channels / dev->sample_bytes;
        
        if (read_frames == 0) {
            pa_threaded_mainloop_unlock(pa_ml);
            return 0;
        }
        dev->dev_latency = read_frames * dev->num_channels * 1000 / (dev->sample_rate / 1000);
        
    }
    
    else {		// Blocking read for dev->read_frames total frames
        WaitForPoll();
        pa_threaded_mainloop_lock(pa_ml);
        read_frames = pa_stream_readable_size(s) / dev->num_channels / dev->sample_bytes;
        
        if (read_frames == 0) {
            pa_threaded_mainloop_unlock(pa_ml);
            return 0;
        }
        dev->dev_latency = read_frames * dev->num_channels * 1000 / (dev->sample_rate / 1000);
    }
    
    
    nSamples = 0;
    
   while (nSamples < read_frames) {		// read samples in chunks until we have enough samples
       if (pa_stream_peek (s, &fbuffer, &read_bytes) < 0) {
           printf("Failure of pa_stream_peek in quisk_read_pulseaudio\n");
           pa_threaded_mainloop_unlock(pa_ml);
           return nSamples;
       }
       
       if (fbuffer == NULL && read_bytes == 0) {		// buffer is empty
           break;
       }
       
       if (fbuffer == NULL) {		// there is a hole in the buffer
           pa_stream_drop(s);
           continue;
       }
       
       if (nSamples * dev->num_channels * dev->sample_bytes + read_bytes >= SAMP_BUFFER_SIZE * 8 / 10) {
           if (quisk_sound_state.verbose_pulse)
               printf("buffer full on %s\n", dev->name);
           pa_stream_drop(s);		// limit read request to buffer size
           break;
       }
       
       // Convert sampled data to complex data. dev->num_channels must be 2.
       if (dev->sample_bytes == 4) {  //float32
           float fi, fq;
           
           for( i = 0; i < (int)read_bytes; i += (dev->num_channels * 4)) {
               memcpy(&fi, fbuffer + i + (dev->channel_I * 4), 4);
               memcpy(&fq, fbuffer + i + (dev->channel_Q * 4), 4);
               if (fi >=  1.0 || fi <= -1.0)
                   dev->overrange++;
               if (fq >=  1.0 || fq <= -1.0)
                   dev->overrange++;
               if (cSamples)
                   cSamples[nSamples] = (fi + I * fq) * CLIP32;
               nSamples++;
           }
       }
       
       else if (dev->sample_bytes == 2) { //16bit integer little-endian
           int16_t si, sq;
           for( i = 0; i < (int)read_bytes; i += (dev->num_channels * 2)) {
               memcpy(&si, fbuffer + i + (dev->channel_I * 2), 2);
               memcpy(&sq, fbuffer + i + (dev->channel_Q * 2), 2);
               if (si >= CLIP16  || si <= -CLIP16)
                   dev->overrange++;
               if (sq >= CLIP16 || sq <= -CLIP16)
                   dev->overrange++;
               int ii = si << 16;
               int qq = sq << 16;
               if (cSamples)
                   cSamples[nSamples] = ii + I * qq;
               nSamples++;
           }
       }
       
       else {
           printf("Unknown sample size for %s", dev->name);
       }
       pa_stream_drop(s);
    }
    pa_threaded_mainloop_unlock(pa_ml);
    return nSamples;
}

/*!
 * \Write outgoing samples directly to pulseaudio server.
 * \param playdev Input. Device to which to play the samples.
 * \param nSamples Input. Number of samples to play.
 * \param cSamples Input. Sample buffer to play from.
 * \param report_latency Input. 1 to update \c quisk_sound_state.latencyPlay, 0 otherwise.
 * \param volume Input. Ratio in [0,1] by which to scale the played samples.
 */
void quisk_play_pulseaudio(struct sound_dev *dev, int nSamples, complex double *cSamples, 
    int report_latency, double volume) {
    pa_stream *s = dev->handle;
    static const pa_timing_info * timing = NULL;
    int i=0, n=0, bufferFrames, bytes_per_frame;
    void *fbuffer;
    size_t fbuffer_bytes = 0;
    size_t writable;

    if( !dev || nSamples <= 0 || dev->pulse_stream_state != PA_STREAM_READY)
        return;
    
    if (dev->cork_status && dev->dev_index != t_Playback)
        return;
    
    if (report_latency) {     // Report the latency, if requested.
        pa_operation *o;
        
        pa_threaded_mainloop_lock(pa_ml);
        
        if (!(o = pa_stream_update_timing_info(s, stream_timing_callback, dev))) {
            printf("pa_stream_update_timing(): %s\n", pa_strerror(pa_context_errno(pa_stream_get_context(s))));
        }
        else {
            while (pa_operation_get_state(o) == PA_OPERATION_RUNNING)
                pa_threaded_mainloop_wait(pa_ml);
            pa_operation_unref(o);
        }
	if (timing == NULL)
		timing = pa_stream_get_timing_info(s);
        
        pa_threaded_mainloop_unlock(pa_ml);
        
    }

    bytes_per_frame = dev->sample_bytes * dev->num_channels;
    if (dev->dev_index == t_Playback) {
    	bufferFrames = (timing->write_index - timing->read_index) / bytes_per_frame;
        if (quisk_sound_state.verbose_pulse > 2)
            printf("read %ld, write %ld, frames %d\n", timing->read_index, timing->write_index, bufferFrames);
        if (dev->cork_status) {
            if (bufferFrames >= dev->latency_frames)
              quisk_cork_pulseaudio(dev, 0);
        }
        else {
            if (underrunPlayback) {
                underrunPlayback = 0;
                quisk_cork_pulseaudio(dev, 1);
            }
        }
    }

    fbuffer = pa_xmalloc(nSamples * bytes_per_frame);
   
    // Convert from complex data to framebuffer

    if (dev->sample_bytes == 4) {
        float fi=0.f, fq=0.f;
        for(i = 0, n = 0; n < nSamples; i += (dev->num_channels * 4), ++n) {
            fi = (volume * creal(cSamples[n])) / CLIP32;
            fq = (volume * cimag(cSamples[n])) / CLIP32;
            memcpy(fbuffer + i + (dev->channel_I * 4), &fi, 4);
            memcpy(fbuffer + i + (dev->channel_Q * 4), &fq, 4);
        }
    }

    else if (dev->sample_bytes == 2) {
        int ii, qq;
        for(i = 0, n = 0; n < nSamples; i += (dev->num_channels * 2), ++n) {
            ii = (int)(volume * creal(cSamples[n]) / 65536);
            qq = (int)(volume * cimag(cSamples[n]) / 65536);
            memcpy(fbuffer + i + (dev->channel_I * 2), &ii, 2);
            memcpy(fbuffer + i + (dev->channel_Q * 2), &qq, 2);
        }
    }

    else {
       printf("Unknown sample size for %s", dev->name);
       exit(15);
    }



    fbuffer_bytes = nSamples * bytes_per_frame;
    pa_threaded_mainloop_lock(pa_ml);
    if (dev->dev_index == t_Playback)	// defeat pa_stream_writable_size()
        writable = 1024 * 1000;
    else
        writable = pa_stream_writable_size(s);

    if (writable > 0) {
        if ( writable > 1024*1000 ) //sanity check to prevent pa_xmalloc from crashing on monitor streams
            writable = 1024*1000;
        if (fbuffer_bytes > writable) {
            if (quisk_sound_state.verbose_pulse && dev->dev_index <= t_MicPlayback)	// ignore for digital interfaces
                printf("Truncating write on %s by %lu bytes\n", dev->stream_description, fbuffer_bytes - writable);
            fbuffer_bytes = writable;
        }
        pa_stream_write(dev->handle, fbuffer, fbuffer_bytes, NULL, 0, PA_SEEK_RELATIVE);
    }
    else {
        if (quisk_sound_state.verbose_pulse && dev->dev_index <= t_MicPlayback)
            printf("Can't write to stream %s. Dropping %lu bytes\n", dev->stream_description, fbuffer_bytes);
    }
    
    pa_threaded_mainloop_unlock(pa_ml);
    pa_xfree(fbuffer);
}

void quisk_pulseaudio_sidetone(struct sound_dev * dev)
{
	int i, ch_I, ch_Q, flags, target_frames, fbuffer_bytes, bytes_per_sample, bytes_per_frame, bufferFrames, writeFrames;
	static play_state_t old_play_state = RECEIVE;
	static const pa_timing_info * timing = NULL;
	pa_operation * o;
	pa_stream * s = dev->handle;	
	void * fbuffer, * buffer;
	void * ptSample;

	if( !dev || dev->pulse_stream_state != PA_STREAM_READY)
		return;

	bytes_per_sample = dev->sample_bytes;
	bytes_per_frame = dev->sample_bytes * dev->num_channels;
	pa_threaded_mainloop_lock(pa_ml);
	if (!(o = pa_stream_update_timing_info(s, stream_timing_callback, dev))) {
		printf("pa_stream_update_timing(): %s\n", pa_strerror(pa_context_errno(pa_stream_get_context(s))));
	}
	else {
		while (pa_operation_get_state(o) == PA_OPERATION_RUNNING)
			pa_threaded_mainloop_wait(pa_ml);
		pa_operation_unref(o);
	}
	if (timing == NULL)
		timing = pa_stream_get_timing_info(s);
	pa_threaded_mainloop_unlock(pa_ml);

	if (timing->read_index_corrupt || timing->write_index_corrupt) {
		printf ("Index corrupt in quisk_pulseaudio_sidetone()\n");
		return;
	}

	bufferFrames = (timing->write_index - timing->read_index) / bytes_per_frame;
       	if (quisk_sound_state.verbose_pulse > 2)
		printf("read %ld, write %ld, frames %d\n", timing->read_index, timing->write_index, bufferFrames);

	if (old_play_state <= RECEIVE && quisk_play_state > RECEIVE) {		// change from Rx to Tx
        	if (quisk_sound_state.verbose_pulse > 2)
			printf("\nChange to Tx\n");
		old_play_state = quisk_play_state;
	}
	else if (old_play_state > RECEIVE && quisk_play_state <= RECEIVE) {	// change from Tx to Rx
        	if (quisk_sound_state.verbose_pulse > 2)
			printf("\nChange to Rx\n");
		dev->old_key = 0;
		old_play_state = quisk_play_state;
        	writeFrames = dev->latency_frames;
		fbuffer_bytes = writeFrames * bytes_per_frame;
		fbuffer = pa_xmalloc(fbuffer_bytes);
		memset(fbuffer, 0, fbuffer_bytes);
		pa_threaded_mainloop_lock(pa_ml);
        	pa_stream_write(s, fbuffer, fbuffer_bytes, NULL, 0, PA_SEEK_RELATIVE_ON_READ);
		pa_threaded_mainloop_unlock(pa_ml);
		pa_xfree(fbuffer);
		return;
	}

	target_frames = tlength_bytes / bytes_per_frame * 4;
	if (QUISK_CWKEY_DOWN != dev->old_key) {	// key changed, re-write buffer
		dev->old_key = QUISK_CWKEY_DOWN;
        	writeFrames = target_frames;
        	if (quisk_sound_state.verbose_pulse > 2)
			printf ("\nChange key %d, writeFrames %d\n", QUISK_CWKEY_DOWN, writeFrames);
		quisk_make_sidetone(dev, bufferFrames);		// rewind sidetone samples
		flags = PA_SEEK_RELATIVE_ON_READ;
	}
	else {		// no change to key; keep buffer full
        	writeFrames = target_frames - bufferFrames;
        	if (quisk_sound_state.verbose_pulse > 2)
			printf("writeFrames %d\n", writeFrames);
		flags = PA_SEEK_RELATIVE;
	}

	if (writeFrames <= 0)
		return;
	fbuffer_bytes = writeFrames * bytes_per_frame;
	fbuffer = pa_xmalloc(fbuffer_bytes);

	buffer = fbuffer;
	ch_I = dev->channel_I;
	ch_Q = dev->channel_Q;
	for (i = 0; i < writeFrames; i++) {
		ptSample = quisk_make_sidetone(dev, 0);
		memcpy(buffer + ch_I * bytes_per_sample, ptSample, bytes_per_sample);
		memcpy(buffer + ch_Q * bytes_per_sample, ptSample, bytes_per_sample);
		buffer += bytes_per_frame;
	}

	pa_threaded_mainloop_lock(pa_ml);
        pa_stream_write(s, fbuffer, fbuffer_bytes, NULL, 0, flags);
	pa_threaded_mainloop_unlock(pa_ml);
	pa_xfree(fbuffer);
}

//This is a function to sort the device list into local and remote lists.
static void sort_devices(struct sound_dev **plist, struct sound_dev **pLocal, struct sound_dev **pRemote) {

    while(*plist) {
        struct sound_dev *dev = *plist++;

	dev->pulse_stream_state = PA_STREAM_UNCONNECTED;
        // Not a PulseAudio device
        if( dev->driver != DEV_DRIVER_PULSEAUDIO )
            continue;

        // Device without name: sad.
        if( !dev->name[0] )
            continue;

        // This is a remote device
        if(dev->server[0]) {
            int i;
            for(i=0;i < PA_LIST_SIZE;i++) {
                if (!(*(pRemote+i))) {
                    *(pRemote+i) = dev;
                    break;
                }
            }
        }

        // This is a local device
        else {
            int i;
            for(i=0;i < PA_LIST_SIZE; i++) {
                if (!(*(pLocal+i))) {
                    *(pLocal+i) = dev;
                    break;
                }
            }
        }
    }
}


/*!
 * \brief Search for and open PulseAudio devices.
 * 
 * \param pCapture Input/Output. Array of capture devices to search through.
 *        If a device has its \c sound_dev.driver field set to
 *        \c DEV_DRIVER_PULSEAUDIO, it will be opened for recording.
 * \param pPlayback Input/Output. Array of playback devices to search through.
 *        If a device has its \c sound_dev.driver field set to
 *        \c DEV_DRIVER_PULSEAUDIO, it will be opened for recording.
 */

//sound_dev ** pointer(address) for list of addresses
//sound_dev * fpointer(address) for list of addresses

void quisk_start_sound_pulseaudio(struct sound_dev **pCapture, struct sound_dev **pPlayback) {
    int num_pa_devices = 0;
    int i;
    //sorted lists of local and remote devices
    struct sound_dev *LocalPulseDevices[PA_LIST_SIZE] = {NULL};
    struct sound_dev *RemotePulseDevices[PA_LIST_SIZE] = {NULL};

    sort_devices(pCapture, LocalPulseDevices, RemotePulseDevices);
    sort_devices(pPlayback, LocalPulseDevices, RemotePulseDevices);
    pa_IQ_ctx = NULL;
    pa_ctx = NULL;
    pa_ml = NULL;
    pa_mlapi = NULL;
    streams_to_start = 0;
    //quisk_sound_state.verbose_pulse = 1;
    
    if (!RemotePulseDevices[0] && !LocalPulseDevices[0]) {
        if (quisk_sound_state.verbose_pulse)
            printf("No pulseaudio devices to open!\n");
        return; //nothing to open. No need to start the mainloop.
    }

    // Create a mainloop API
    pa_ml = pa_threaded_mainloop_new();
    pa_mlapi = pa_threaded_mainloop_get_api(pa_ml);

    assert(pa_signal_init(pa_mlapi) == 0);
  
    if (pa_threaded_mainloop_start(pa_ml) < 0) {
        printf("pa_mainloop_run() failed.");
	return;
    }
    else
        printf("Pulseaudio threaded mainloop started\n");
  
    pa_threaded_mainloop_lock(pa_ml);

    if (RemotePulseDevices[0]) {	//we've got at least 1 remote device
        pa_IQ_ctx = pa_context_new(pa_mlapi, "Quisk-remote");
        if (pa_context_connect(pa_IQ_ctx, quisk_sound_state.IQ_server, 0, NULL) < 0)
            printf("Failed to connect to remote Pulseaudio server\n");
        pa_context_set_state_callback(pa_IQ_ctx, state_cb, RemotePulseDevices); //send a list of remote devices to open
    }

    if (LocalPulseDevices[0]) {	//we've got at least 1 local device
        pa_ctx = pa_context_new(pa_mlapi, "Quisk-local");
        if (pa_context_connect(pa_ctx, NULL, 0, NULL) < 0)
            printf("Failed to connect to local Pulseaudio server\n");
        pa_context_set_state_callback(pa_ctx, state_cb, LocalPulseDevices);
    }


    pa_threaded_mainloop_unlock(pa_ml);

    for(i=0; LocalPulseDevices[i]; i++) {
            num_pa_devices++;
    }

    for(i=0; RemotePulseDevices[i]; i++) {
            num_pa_devices++;
    }

    if (quisk_sound_state.verbose_pulse)
        printf("Waiting for %d streams to start\n", num_pa_devices);
    
    while (streams_to_start < num_pa_devices); // wait for all the devices to open

    if (quisk_sound_state.verbose_pulse)
        printf("All streams started\n");
    

}


// Close all streams/contexts/loops and return
void quisk_close_sound_pulseaudio() {
    int i = 0;
    
    if (quisk_sound_state.verbose_pulse)
        printf("Closing Pulseaudio interfaces \n ");

    while (OpenPulseDevices[i]) {
        pa_stream_disconnect(OpenPulseDevices[i]);
        pa_stream_unref(OpenPulseDevices[i]);
        OpenPulseDevices[i] = '\0';
        i++;
    }
    

    if (quisk_sound_state.verbose_pulse)
        printf("Waiting for %d streams to disconnect\n", streams_ready);

    while(streams_ready > 0);

    if (pa_IQ_ctx) {
        pa_context_disconnect(pa_IQ_ctx);
        pa_context_unref(pa_IQ_ctx);
	pa_IQ_ctx = NULL;
    }

    if (pa_ctx) {
        pa_context_disconnect(pa_ctx);
        pa_context_unref(pa_ctx);
	pa_ctx = NULL;
    }

    if (pa_ml) {
        pa_threaded_mainloop_stop(pa_ml);
        pa_threaded_mainloop_free(pa_ml);
	pa_ml = NULL;
    }
}



// Additional bugs added by N2ADR below this point.
// Code for quisk_pulseaudio_sound_devices is based on code by Igor Brezac and Eric Connell, and Jan Newmarch.
// This should only be called when Quisk first starts, but names changed to protect the other mainloop.

// This callback gets called when our context changes state.  We really only
// care about when it's ready or if it has failed.
static void pa_names_state_cb(pa_context *c, void *userdata) {
	pa_context_state_t ctx_state;
	int *main_state = userdata;

	ctx_state = pa_context_get_state(c);
	switch  (ctx_state) {
		// There are just here for reference
		case PA_CONTEXT_UNCONNECTED:
		case PA_CONTEXT_CONNECTING:
		case PA_CONTEXT_AUTHORIZING:
		case PA_CONTEXT_SETTING_NAME:
		default:
			break;
		case PA_CONTEXT_FAILED:
		case PA_CONTEXT_TERMINATED:
			*main_state = 9;
			break;
		case PA_CONTEXT_READY:
			*main_state = 1;
			break;
	}
}

static void source_sink(const char * name, const char * descr, pa_proplist * props, PyObject * pylist) {
	const char * value;
	char buf300[300];
	PyObject * pytup;

	pytup = PyTuple_New(3);
	PyList_Append(pylist, pytup);
	PyTuple_SET_ITEM(pytup, 0, PyString_FromString(name));
	PyTuple_SET_ITEM(pytup, 1, PyString_FromString(descr));
	value = pa_proplist_gets(props, "device.api");
    
	if (value && ! strcmp(value, "alsa")) {
		snprintf(buf300, 300, "%s %s (hw:%s,%s)", pa_proplist_gets(props, "alsa.card_name"), pa_proplist_gets(props, "alsa.name"),
			 pa_proplist_gets(props, "alsa.card"), pa_proplist_gets(props, "alsa.device"));

		PyTuple_SET_ITEM(pytup, 2, PyString_FromString(buf300));
	}
	else {
		PyTuple_SET_ITEM(pytup, 2, PyString_FromString(""));
	}
}




// pa_mainloop will call this function when it's ready to tell us about a sink.
static void pa_sinklist_cb(pa_context *c, const pa_sink_info *l, int eol, void *userdata) {
    if (eol > 0)	// If eol is set to a positive number, you're at the end of the list
		return;
    source_sink(l->name, l->description, l->proplist, (PyObject *)userdata);
    if ( ! strcmp(l->name, "QuiskDigitalInput"))
        have_QuiskDigitalInput = 1;
    if ( ! strcmp(l->name, "QuiskDigitalOutput"))
        have_QuiskDigitalOutput = 1;
}

static void pa_sourcelist_cb(pa_context *c, const pa_source_info *l, int eol, void *userdata) {
    if (eol > 0)
		return;
    source_sink(l->name, l->description, l->proplist, (PyObject *)userdata);
}

static void index_callback(pa_context *c, uint32_t idx, void *userdata) {
    //printf("%u\n", idx);
}

PyObject * quisk_pulseaudio_sound_devices(PyObject * self, PyObject * args)
{	// Return a list of PulseAudio device names [pycapt, pyplay]
	PyObject * pylist, * pycapt, * pyplay;
	pa_mainloop *pa_names_ml;
	pa_mainloop_api *pa_names_mlapi;
	pa_operation *pa_op=NULL;
	pa_context *pa_names_ctx;
	int state = 0;

	if (!PyArg_ParseTuple (args, ""))
		return NULL;
	// Each pycapt and pyplay is (dev name, description, alsa name)
	pylist = PyList_New(0);		// list [pycapt, pyplay]
	pycapt = PyList_New(0);		// list of capture devices
	pyplay = PyList_New(0);		// list of play devices
	PyList_Append(pylist, pycapt);
	PyList_Append(pylist, pyplay);
    
    //printf("Starting name loop\n");

	// Create a mainloop API and connection to the default server
	pa_names_ml = pa_mainloop_new();
	pa_names_mlapi = pa_mainloop_get_api(pa_names_ml);
	pa_names_ctx = pa_context_new(pa_names_mlapi, "DeviceNames");
 
	// This function connects to the pulse server
	if (pa_context_connect(pa_names_ctx, NULL, 0, NULL) < 0) {
	   if (quisk_sound_state.verbose_pulse)
	      printf("No local daemon to connect to for show_pulse_audio_devices option\n");
	   return pylist;
	}

	// This function defines a callback so the server will tell us it's state.
	pa_context_set_state_callback(pa_names_ctx, pa_names_state_cb, &state);

	// Now we'll enter into an infinite loop until we get the data we receive or if there's an error
	while (state < 10) {
		switch (state) {
		case 0:	// We can't do anything until PA is ready
			pa_mainloop_iterate(pa_names_ml, 1, NULL);
			break;
		case 1:
			// This sends an operation to the server.  pa_sinklist_info is
			// our callback function and a pointer to our devicelist will
			// be passed to the callback.
			pa_op = pa_context_get_sink_info_list(pa_names_ctx, pa_sinklist_cb, pyplay);
			// Update state for next iteration through the loop
			state++;
			pa_mainloop_iterate(pa_names_ml, 1, NULL);
			break;
		case 2:
			// Now we wait for our operation to complete.  When it's
			// complete our pa_output_devicelist is filled out, and we move
			// along to the next state
			if (pa_operation_get_state(pa_op) == PA_OPERATION_DONE) {
				pa_operation_unref(pa_op);
				// Now we perform another operation to get the source
				// (input device) list just like before.
				pa_op = pa_context_get_source_info_list(pa_names_ctx, pa_sourcelist_cb, pycapt);
				// Update the state so we know what to do next
				state++;
			}
			pa_mainloop_iterate(pa_names_ml, 1, NULL);
			break;
		case 3:
			if (pa_operation_get_state(pa_op) == PA_OPERATION_DONE) {
				pa_operation_unref(pa_op);
				state = 4;
			}
			else
				pa_mainloop_iterate(pa_names_ml, 1, NULL);
			break;
		// The following loads modules for digital input and output if they are not present.
		case 4:
			if ( ! have_QuiskDigitalInput) {
				pa_op = pa_context_load_module(pa_names_ctx, "module-null-sink",
					"sink_name=QuiskDigitalInput sink_properties=device.description=QuiskDigitalInput",
					index_callback, NULL);
				state = 5;
				pa_mainloop_iterate(pa_names_ml, 1, NULL);
			}
			else
				state = 6;
			break;
		case 5:
			if (pa_operation_get_state(pa_op) == PA_OPERATION_DONE) {
				pa_operation_unref(pa_op);
				state = 6;
			}
			else
				pa_mainloop_iterate(pa_names_ml, 1, NULL);
			break;
		case 6:
			if ( ! have_QuiskDigitalOutput) {
				pa_op = pa_context_load_module(pa_names_ctx, "module-null-sink",
					"sink_name=QuiskDigitalOutput sink_properties=device.description=QuiskDigitalOutput",
					index_callback, NULL);
				state = 7;
				pa_mainloop_iterate(pa_names_ml, 1, NULL);
			}
			else
				state = 9;
			break;
		case 7:
			if (pa_operation_get_state(pa_op) == PA_OPERATION_DONE) {
				pa_operation_unref(pa_op);
				state = 9;
			}
			else
				pa_mainloop_iterate(pa_names_ml, 1, NULL);
			break;
		case 9:				// Now we're done, clean up and disconnect and return
			pa_context_disconnect(pa_names_ctx);
			pa_context_unref(pa_names_ctx);
			pa_mainloop_free(pa_names_ml);
			state = 99;
			break;
		}
	}
    //printf("Finished with name loop\n");
	return pylist;
}
#else		// No PulseAudio available
#include <Python.h>
#include <complex.h>
#include "quisk.h"

PyObject * quisk_pulseaudio_sound_devices(PyObject * self, PyObject * args)
{	// Return a list of PulseAudio device names [pycapt, pyplay]
	return quisk_dummy_sound_devices(self, args);
}

void quisk_start_sound_pulseaudio(struct sound_dev **pCapture, struct sound_dev **pPlayback)
{
	struct sound_dev * pDev;
	const char * msg = "No driver support for this device";

	while (*pCapture) {
		pDev = *pCapture++;
		if (pDev->driver == DEV_DRIVER_PULSEAUDIO) {
			strMcpy(pDev->dev_errmsg, msg, QUISK_SC_SIZE);
			if (quisk_sound_state.verbose_sound)
				QuiskPrintf("%s\n", msg);
		}
	}
	while (*pPlayback) {
		pDev = *pPlayback++;
		if (pDev->driver == DEV_DRIVER_PULSEAUDIO) {
			strMcpy(pDev->dev_errmsg, msg, QUISK_SC_SIZE);
			if (quisk_sound_state.verbose_sound)
				QuiskPrintf("%s\n", msg);
		}
	}
}

int quisk_read_pulseaudio(struct sound_dev *dev, complex double *cSamples)
{
	return 0;
}

void quisk_play_pulseaudio(struct sound_dev *dev, int nSamples, complex double *cSamples, int report_latency, double volume)
{}

void quisk_close_sound_pulseaudio() 
{}

void quisk_cork_pulseaudio(struct sound_dev *dev, int b) 
{}

void quisk_flush_pulseaudio(struct sound_dev *dev) 
{}
#endif
