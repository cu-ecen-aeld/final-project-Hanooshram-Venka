/*
 * File: main.cpp
 * Author: Hanooshram Venkateswaran
 * Issues covered:
 * #5 - Thread-Safe Circular Buffer Integration
 * #6 - Shared DSP Configuration via IPC
 * #7 - SoundTouch DSP Integration
 * #9 - A-to-B Audio Looping Logic
 * #10 - Final System Validation & Bounds Checking (Current Focus for Issue #10)
 *
 * Brief:
 * - Implements multithreaded WAV playback with SoundTouch DSP.
 * - Issue #10 Implementation: Introduces relational bounds checking for looping
 * commands (ensuring Point A < Point B and within total track duration), hard limits
 * for pitch/tempo, and dynamic Play/Pause/Quit execution via the TCP socket server.
 * - Includes signal handling (SIGINT/SIGTERM) for graceful teardown on Ctrl+C.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <pthread.h>
#include <sndfile.h>
#include <alsa/asoundlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <signal.h> // Added for signal handling
#include <soundtouch/SoundTouch.h>

#define PCM_DEVICE "default"
#define CHUNK_FRAMES 4096
#define RING_BUFFER_CHUNKS 10
#define PORT 9000
#define BUFFER_SIZE 1024

// --- IPC Shared State ---
// All threads can see this shared state as this struct lives in the global RAM
typedef struct {
    float tempo;
    float pitch;

    // Looping State
    bool loop_enabled;
    float loop_a_sec;
    float loop_b_sec;

    // Issue #10: Playback Control States
    bool is_paused;

    pthread_mutex_t lock; // Mutex to protect shared state from concurrent access.
} DSPConfig;

DSPConfig shared_config;

// Circular Buffer Structure
// This data structure allows asynchronous data transfer Producer and Consumer threads
typedef struct {
    short *data;
    int head; // Write pointer index
    int tail; // Read pointer inder
    int count; // Current volume of frmaes in the buffer
    int max_frames; // Frame capacity of the buffer
    int channels; // No. of Audio Channels
    bool eof_reached; //  This flag indicates completion of file reading

    // Issue #10: Immediate hardware shutdown trigger
    bool force_quit;

    pthread_mutex_t lock;
    pthread_cond_t not_empty; // Condition variable to signal data availability
    pthread_cond_t not_full; // To signal Space availability
} AudioRingBuffer;

AudioRingBuffer ring_buf;
SNDFILE *infile;
snd_pcm_t *pcm_handle;
soundtouch::SoundTouch *pSoundTouch;

// Global audio metadata for time-to-frame conversions and Issue #10 bounds checking
int g_sample_rate = 48000;
float g_total_duration_sec = 0.0f;

// --- SIGNAL HANDLER ---
// Catches Ctrl+C (SIGINT) or termination signals to safely shut down ALSA
static void signal_handler(int signum) {
    if (signum == SIGINT || signum == SIGTERM) {
        printf("\n[System] Caught signal %d. Initiating graceful hardware teardown...\n", signum);

        // Safely force EOF conditions to break Producer and Consumer out of their loops
        pthread_mutex_lock(&ring_buf.lock);
        ring_buf.force_quit = true;
        ring_buf.eof_reached = true;
        pthread_cond_broadcast(&ring_buf.not_empty);
        pthread_cond_broadcast(&ring_buf.not_full);
        pthread_mutex_unlock(&ring_buf.lock);
    }
}

// PRODUCER THREAD
void *producer_thread_func(void *arg) {
    short *temp_buf = (short *)malloc(CHUNK_FRAMES * ring_buf.channels * sizeof(short));
    sf_count_t current_file_pos = 0;

    while (1) {
        // Safely fetch looping parameters
        pthread_mutex_lock(&shared_config.lock);
        bool looping = shared_config.loop_enabled;
        sf_count_t loop_start = (sf_count_t)(shared_config.loop_a_sec * g_sample_rate);
        sf_count_t loop_end = (sf_count_t)(shared_config.loop_b_sec * g_sample_rate);
        pthread_mutex_unlock(&shared_config.lock);

        // Calculates how many frames are allowed to be read before hitting Point B
        sf_count_t frames_to_read = CHUNK_FRAMES;
        if (looping && loop_end > loop_start) {
            if (current_file_pos + CHUNK_FRAMES > loop_end) {
                frames_to_read = loop_end - current_file_pos;
                if (frames_to_read <= 0) frames_to_read = 0;
            }
        }

        sf_count_t frames_read = 0;
        if (frames_to_read > 0) {
            frames_read = sf_readf_short(infile, temp_buf, frames_to_read);
        }

        // Push successfully read frames into the Ring Buffer
        if (frames_read > 0) {
            pthread_mutex_lock(&ring_buf.lock);

            // Wait while buffer is full, but break immediately if a quit command is received
            while (ring_buf.count + frames_read > ring_buf.max_frames && !ring_buf.force_quit) {
                pthread_cond_wait(&ring_buf.not_full, &ring_buf.lock);
            }

            if (ring_buf.force_quit) {
                pthread_mutex_unlock(&ring_buf.lock);
                break;
            }

            // Write data chunk to the ring buffer, handling index wrap-around
            int total_samples = frames_read * ring_buf.channels;
            for (int i = 0; i < total_samples; i++) {
                ring_buf.data[ring_buf.head] = temp_buf[i];
                ring_buf.head = (ring_buf.head + 1) % (ring_buf.max_frames * ring_buf.channels);
            }

            ring_buf.count += frames_read;
            pthread_cond_signal(&ring_buf.not_empty); // Signal the consumer thread that new data is available for processing
            pthread_mutex_unlock(&ring_buf.lock);

            current_file_pos += frames_read;
        }

        // Looping or EOF detection
        if (frames_read == 0 || (looping && current_file_pos >= loop_end)) {
            if (looping && loop_end > loop_start) {
                // Instantly rewinds the file pointer to Point A
                sf_seek(infile, loop_start, SEEK_SET);
                current_file_pos = loop_start;
                continue; // Skip the EOF break and keep reading
            } else {
                break; // Standard EOF
            }
        }
    }

    pthread_mutex_lock(&ring_buf.lock);
    ring_buf.eof_reached = true;
    pthread_cond_broadcast(&ring_buf.not_empty);
    pthread_mutex_unlock(&ring_buf.lock);

    free(temp_buf);
    printf("[Producer] Finished reading file.\n");
    return NULL;
}

// CONSUMER THREAD
// Function of this thread -Dequeue audio frames from the circular buffer and write them to the LSA PCM hardware driver
void *consumer_thread_func(void *arg) {

    // 16-bit Integer buffers for the ring buffer and ALSA
    short *temp_buf = (short *)malloc(CHUNK_FRAMES * ring_buf.channels * sizeof(short));
    short *dsp_out_buf = (short *)malloc(CHUNK_FRAMES * ring_buf.channels * sizeof(short));

    // 32-bit Float buffers for SoundTouch DSP processing
    float *float_in_buf = (float *)malloc(CHUNK_FRAMES * ring_buf.channels * sizeof(float));
    float *float_out_buf = (float *)malloc(CHUNK_FRAMES * ring_buf.channels * sizeof(float));

    while (1) {
 
        // Issue #10: Check Play/Pause state. Sleeping briefly prevents 100% CPU lock while paused.
        pthread_mutex_lock(&shared_config.lock);
        bool is_paused = shared_config.is_paused;
        pthread_mutex_unlock(&shared_config.lock);

        if (is_paused) {
            usleep(50000); // 50ms polling rest
            continue;
        }

        pthread_mutex_lock(&ring_buf.lock);
        while (ring_buf.count < CHUNK_FRAMES && !ring_buf.eof_reached && !ring_buf.force_quit) { 
            pthread_cond_wait(&ring_buf.not_empty, &ring_buf.lock);
        }

        if (ring_buf.force_quit) {
            pthread_mutex_unlock(&ring_buf.lock);
            break;
        }

        if (ring_buf.count == 0 && ring_buf.eof_reached) { // Terminate execution loop upon reaching EOF
            pthread_mutex_unlock(&ring_buf.lock);

            // Flush any remaining buffered samples out of the DSP engine
            pSoundTouch->flush();
            int flushed_frames;
            while ((flushed_frames = pSoundTouch->receiveSamples(float_out_buf, CHUNK_FRAMES)) > 0) {

                // Convert flushed floats back to 16-bit integers for ALSA
                int total_flushed_samples = flushed_frames * ring_buf.channels;
                for (int i = 0; i < total_flushed_samples; i++) {
                    float val = float_out_buf[i] * 32768.0f;
                    if (val > 32767.0f) val = 32767.0f; // Hard Clipping protection
                    if (val < -32768.0f) val = -32768.0f;
                    dsp_out_buf[i] = (short)val;
                }
                snd_pcm_writei(pcm_handle, dsp_out_buf, flushed_frames);
            }
            break;
        }

        int frames_to_pull = (ring_buf.count < CHUNK_FRAMES) ? ring_buf.count : CHUNK_FRAMES;
        int total_samples = frames_to_pull * ring_buf.channels;

        for (int i = 0; i < total_samples; i++) {
            temp_buf[i] = ring_buf.data[ring_buf.tail];
            ring_buf.tail = (ring_buf.tail + 1) % (ring_buf.max_frames * ring_buf.channels);
        }

        ring_buf.count -= frames_to_pull;
        pthread_cond_signal(&ring_buf.not_full);
        pthread_mutex_unlock(&ring_buf.lock);

        // --- DSP PIPELINE ---

        // Safely grab the latest network commands
        pthread_mutex_lock(&shared_config.lock);
        float current_tempo = shared_config.tempo;
        float current_pitch = shared_config.pitch;
        pthread_mutex_unlock(&shared_config.lock);

        // 2. Update DSP Engine dynamically
        pSoundTouch->setTempo(current_tempo);
        pSoundTouch->setPitch(current_pitch);

        // 3. DSP BRIDGE IN: Convert 16-bit ints to 32-bit floats (-1.0 to 1.0)
        for (int i = 0; i < total_samples; i++) {
            float_in_buf[i] = (float)temp_buf[i] / 32768.0f;
        }

        // 4. Push normalized float frames to the DSP
        pSoundTouch->putSamples(float_in_buf, frames_to_pull);

        // 5. Continuously drain processed float frames from the DSP
        int processed_frames;
        while ((processed_frames = pSoundTouch->receiveSamples(float_out_buf, CHUNK_FRAMES)) > 0) {

            // 6. DSP BRIDGE OUT: Convert 32-bit floats back to 16-bit ints for ALSA
            int processed_samples = processed_frames * ring_buf.channels;
            for (int i = 0; i < processed_samples; i++) {
                float val = float_out_buf[i] * 32768.0f;
                if (val > 32767.0f) val = 32767.0f; // Hard Clipping protection
                if (val < -32768.0f) val = -32768.0f;
                dsp_out_buf[i] = (short)val;
            }

            // 7. Write the converted data to the ALSA hardware loop
            snd_pcm_sframes_t frames_written = snd_pcm_writei(pcm_handle, dsp_out_buf, processed_frames);
            if (frames_written < 0) {
                snd_pcm_prepare(pcm_handle);
            }
        }
    }

    free(temp_buf);
    free(dsp_out_buf);
    free(float_in_buf);
    free(float_out_buf);
    printf("[Consumer] Finished playing audio.\n");
    return NULL;
}

// CONTROL THREAD

void *control_thread_func(void *arg) {
    int server_fd, new_socket;
    struct sockaddr_in address;
    int opt = 1;
    int addrlen = sizeof(address);
    char buffer[BUFFER_SIZE] = {0};

    // Socket initialization and allocation
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("[Control] Socket failed");
        return NULL;
    }
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0){
       perror("[Control] Bind failed");
       return NULL;
   }

    if (listen(server_fd, 3) < 0) {
       perror("[Control] Listen failed");
       return NULL;
   }

    printf("[Control] Server listening on Port %d\n", PORT);
    // Keep accepting incoming TCP client connections until file playback reaches EOF
    while (!ring_buf.eof_reached) {
        // Accept a new client connection on the listening socket
        new_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen);
        // If accept fails, skip this iteration and keep the server alive
        if (new_socket < 0) continue;

        // Read the incoming command string from the connected client into the buffer
        int valread = read(new_socket, buffer, BUFFER_SIZE);
        if (valread > 0) {
            char param[50];
            float val1 = 0.0f, val2 = 0.0f;

            // Modified to parse up to TWO float values simultaneously 
            int parsed_args = sscanf(buffer, "set %s %f %f", param, &val1, &val2);

            if (parsed_args >= 2) {
                pthread_mutex_lock(&shared_config.lock);

                // Issue #10: Relational Bounds Checking Logic applied to all parameters
                if (strcmp(param, "tempo") == 0) {
                    if (val1 < 0.5f || val1 > 2.0f) {
                        printf("[Control] Error: Tempo %.2f out of bounds (0.5 - 2.0).\n", val1);
                    } else {
                        shared_config.tempo = val1;
                        printf("[Control] Tempo updated to %.2f\n", val1);
                    }
                } else if (strcmp(param, "pitch") == 0) {
                    if (val1 < 0.5f || val1 > 2.0f) {
                        printf("[Control] Error: Pitch %.2f out of bounds (0.5 - 2.0).\n", val1);
                    } else {
                        shared_config.pitch = val1;
                        printf("[Control] Pitch updated to %.2f\n", val1);
                    }
                } else if (strcmp(param, "loop") == 0 && parsed_args == 3) {
                    // Atomic loop command: Checks A, B, and Track Duration all at once
                    if (val1 < 0.0f || val1 >= val2 || val2 > g_total_duration_sec) {
                        printf("[Control] Error: Invalid bounds A:%.2f B:%.2f. Track is %.2fs. Rejected.\n", val1, val2, g_total_duration_sec);
                    } else {
                        shared_config.loop_a_sec = val1;
                        shared_config.loop_b_sec = val2;
                        shared_config.loop_enabled = true;
                        printf("[Control] A-to-B Looping ON (A: %.2f | B: %.2f)\n", val1, val2);
                    }
                } else if (strcmp(param, "loop_off") == 0) {
                    shared_config.loop_enabled = false;
                    printf("[Control] A-to-B Looping is now OFF\n");
                } else if (strcmp(param, "pause") == 0) {
                    shared_config.is_paused = (val1 >= 1.0f);
                    printf("[Control] Playback state: %s\n", shared_config.is_paused ? "PAUSED" : "PLAYING");
                } else if (strcmp(param, "quit") == 0) {
                    printf("[Control] Quit signal received. Initiating graceful hardware teardown...\n");

                    // Safely force EOF conditions to break Producer and Consumer out of their loops
                    pthread_mutex_lock(&ring_buf.lock);
                    ring_buf.force_quit = true;
                    ring_buf.eof_reached = true;
                    pthread_cond_broadcast(&ring_buf.not_empty);
                    pthread_cond_broadcast(&ring_buf.not_full);
                    pthread_mutex_unlock(&ring_buf.lock);
                } else {
                    printf("[Control] Unknown parameter: %s\n", param);
                }

                pthread_mutex_unlock(&shared_config.lock);
            }
            memset(buffer, 0, BUFFER_SIZE);
        }
        close(new_socket);
    }
    close(server_fd);
    return NULL;
}



// --- MAIN ROUTINE ---
int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <input_audio.wav>\n", argv[0]);
        return EXIT_FAILURE;
    }

    // Register signal handlers for graceful shutdown (Ctrl+C)
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    // Opens WAV file and extracts structural metadata
    SF_INFO sfinfo;
    sfinfo.format = 0;
    infile = sf_open(argv[1], SFM_READ, &sfinfo);
    if (!infile) return EXIT_FAILURE;

    // Issue #10: Calculate total file duration in seconds for bounds checking
    g_sample_rate = sfinfo.samplerate;
    g_total_duration_sec = (float)sfinfo.frames / sfinfo.samplerate; 

    // Initialize Shared DSP Config
    shared_config.tempo = 1.0f;
    shared_config.pitch = 1.0f;
    shared_config.loop_enabled = false;
    shared_config.loop_a_sec = 0.0f;
    shared_config.loop_b_sec = g_total_duration_sec; 
    shared_config.is_paused = false;
    pthread_mutex_init(&shared_config.lock, NULL);

    // Initialize and configure the ALSA PCM Hardware Interface
    if (snd_pcm_open(&pcm_handle, PCM_DEVICE, SND_PCM_STREAM_PLAYBACK, 0) < 0) return EXIT_FAILURE;
    snd_pcm_set_params(pcm_handle, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, sfinfo.channels, sfinfo.samplerate, 1, 500000);

    // Allocate memory and initialize synchronization for the Circular Buffer
    ring_buf.max_frames = CHUNK_FRAMES * RING_BUFFER_CHUNKS;
    ring_buf.channels = sfinfo.channels;
    ring_buf.data = (short *)malloc(ring_buf.max_frames * ring_buf.channels * sizeof(short)); 
    ring_buf.head = ring_buf.tail = ring_buf.count = 0;
    ring_buf.eof_reached = false;
    ring_buf.force_quit = false;

    pthread_mutex_init(&ring_buf.lock, NULL);
    pthread_cond_init(&ring_buf.not_empty, NULL);
    pthread_cond_init(&ring_buf.not_full, NULL);

    // --- INITIALIZE DSP ENGINE (Using native C++ initialization) ---
    pSoundTouch = new soundtouch::SoundTouch();
    pSoundTouch->setSampleRate(sfinfo.samplerate);
    pSoundTouch->setChannels(sfinfo.channels);
    pSoundTouch->setTempo(1.0f);
    pSoundTouch->setPitch(1.0f);

    printf("--- Starting Multithreaded Audio Engine ---\n");
    printf("--- Track Loaded: %.2f seconds total duration ---\n", g_total_duration_sec);

    // Release concurrent execution threads
    pthread_t producer, consumer, control;
    pthread_create(&producer, NULL, producer_thread_func, NULL);
    pthread_create(&consumer, NULL, consumer_thread_func, NULL);
    pthread_create(&control, NULL, control_thread_func, NULL);

    // Blocks main execution until the primary operations threads temrinate
    pthread_join(producer, NULL);
    pthread_join(consumer, NULL);

    // Execute graceful shutdown of hardware peripherals, memory, and synchronization variables
    delete pSoundTouch;
    snd_pcm_drain(pcm_handle);
    snd_pcm_close(pcm_handle);
    sf_close(infile);
    free(ring_buf.data);

    pthread_mutex_lock(&ring_buf.lock); // Lock before destroying
    pthread_mutex_unlock(&ring_buf.lock);
    pthread_mutex_destroy(&ring_buf.lock);
    pthread_cond_destroy(&ring_buf.not_empty);
    pthread_cond_destroy(&ring_buf.not_full);
    pthread_mutex_destroy(&shared_config.lock);

    printf("--- System Shut Down Cleanly ---\n");
    return EXIT_SUCCESS;
}
