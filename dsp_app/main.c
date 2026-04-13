
/*
 * File: main.c
 *
 * Brief: Multithreaded WAV parser, ALSA playback, and TCP Control Server.
 * Addresses Issues #5 and #6.
 *
 * Description - Implements a Producer-Consumer architecture using POSIX threads
 * and a mutex protected circular buffer. The Producer threads role is to gather/geneate data and write to RAM
 * The Consumer threads' role is to pull audio frames from RAM and feed them to the ALSA hardware driver
 * The Cirbular buffer is the shared memory queue that is inbetween the producer and consumer threads.
 *
 * Author - Hanooshram Venkateswaran
 *
 * References:
 * 1) Used structure and logic from course ECEN5713 for the following - aesdsocket.c (from Assigment 5/6), Multithreading concepts from Assignment 6, and circular buffer implementation from assignment 8 
 * 2) Linux man pages for syntax
 * 3) The ALSA Project API documentation - mainly for the hardware playback loop
 * 4) Beej's Guide to Network Programming - for socket daemon parts of the code
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

#define PCM_DEVICE "default"
#define CHUNK_FRAMES 4096
#define RING_BUFFER_CHUNKS 10
#define PORT 9000
#define BUFFER_SIZE 1024

// IPC Shared State (Issue #6)
//ALl threads can see this shared state as this struct lives in the global RAM
typedef struct {
    float tempo;
    float pitch;
    pthread_mutex_t lock; // Mutex to protect shared state from concurrent access.
} DSPConfig;

DSPConfig shared_config;

// Circular Buffer Structure
//This data structure allows asynchronous data transfer Producer and Consumer threads
typedef struct {
    short *data;
    int head;  // Write pointer index
    int tail;  // Read pointer inder
    int count;  // Current volume of frmaes in the buffer
    int max_frames;  // Frame capacity of the buffer
    int channels;  // No. of Audio Channels
    bool eof_reached; // This flag indicates completion of file reading

    pthread_mutex_t lock;
    pthread_cond_t not_empty; // Condition variable to signal data availability
    pthread_cond_t not_full; // To signal Space availability
} AudioRingBuffer;

AudioRingBuffer ring_buf;
SNDFILE *infile;
snd_pcm_t *pcm_handle;


// PRODUCER THREAD
//Function of this thread - read audio frames from the file system and enqueue them, blocks when the buffer reaches maximum capacity
void *producer_thread_func(void *arg) {
    short *temp_buf = malloc(CHUNK_FRAMES * ring_buf.channels * sizeof(short));
    sf_count_t frames_read;

    // Loops until the end of the input file is reached
    while ((frames_read = sf_readf_short(infile, temp_buf, CHUNK_FRAMES)) > 0) {
        pthread_mutex_lock(&ring_buf.lock);

        //Block thread execution if the buffer lacks sufficient space
        while (ring_buf.count + frames_read > ring_buf.max_frames) {
            pthread_cond_wait(&ring_buf.not_full, &ring_buf.lock);
        }

        // Write data chunk to the ring buffer, handling index wrap-around
        int total_samples = frames_read * ring_buf.channels;
        for (int i = 0; i < total_samples; i++) {
            ring_buf.data[ring_buf.head] = temp_buf[i];
            ring_buf.head = (ring_buf.head + 1) % (ring_buf.max_frames * ring_buf.channels);
        }

        ring_buf.count += frames_read;
        pthread_cond_signal(&ring_buf.not_empty); //Signal the consumer thread that new data is available for processing
        pthread_mutex_unlock(&ring_buf.lock);
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
    short *temp_buf = malloc(CHUNK_FRAMES * ring_buf.channels * sizeof(short));

    while (1) {
        pthread_mutex_lock(&ring_buf.lock);
        while (ring_buf.count < CHUNK_FRAMES && !ring_buf.eof_reached) { //Block thread execution if the buffer is empty and active streaming is ongoing
            pthread_cond_wait(&ring_buf.not_empty, &ring_buf.lock);
        }

        if (ring_buf.count == 0 && ring_buf.eof_reached) { // Terminate execution loop upon reaching EOF
            pthread_mutex_unlock(&ring_buf.lock);
            break; 
        }
       // Determines read size to account for potential fractional data chunks at EOF
        int frames_to_pull = (ring_buf.count < CHUNK_FRAMES) ? ring_buf.count : CHUNK_FRAMES;
        int total_samples = frames_to_pull * ring_buf.channels;

        // Read data chunk from the ring buffer, handling index wrap-around
        for (int i = 0; i < total_samples; i++) {
            temp_buf[i] = ring_buf.data[ring_buf.tail];
            ring_buf.tail = (ring_buf.tail + 1) % (ring_buf.max_frames * ring_buf.channels);
        }

        ring_buf.count -= frames_to_pull;
        pthread_cond_signal(&ring_buf.not_full);
        pthread_mutex_unlock(&ring_buf.lock);

        // Reserved space here for sprint 3 issues


        // Interface with ALSA API for hardware playback. Handled outside of mutex to prevent blocking.
        snd_pcm_sframes_t frames_written = snd_pcm_writei(pcm_handle, temp_buf, frames_to_pull);
        if (frames_written < 0) {
            snd_pcm_prepare(pcm_handle); // ALSA hardware recovery routine for buffer underrun
        }
    }

    free(temp_buf);
    printf("[Consumer] Finished playing audio.\n");
    return NULL;
}

// CONTROL THREAD (Socket Server)
// Function - Maintain a listening TCP socket to receive string commands, parse them,
// and safely update the global DSP configuration variables.
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

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("[Control] Bind failed");
        return NULL;
    }
    if (listen(server_fd, 3) < 0) {
        perror("[Control] Listen failed");
        return NULL;
    }

    printf("[Control] Server listening on Port %d\n", PORT);

    while (!ring_buf.eof_reached) {
        new_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen);
        if (new_socket < 0) continue;

        int valread = read(new_socket, buffer, BUFFER_SIZE);
        if (valread > 0) {
            char param[50];
            float value;

            // Parse the incoming string (e.g., "set tempo 1.2")
            if (sscanf(buffer, "set %s %f", param, &value) == 2) {
                // Safely update the shared state
                pthread_mutex_lock(&shared_config.lock);
                if (strcmp(param, "tempo") == 0) {
                    shared_config.tempo = value;
                    printf("[Control] Tempo updated to %.2f\n", value);
                } else if (strcmp(param, "pitch") == 0) {
                    shared_config.pitch = value;
                    printf("[Control] Pitch updated to %.2f\n", value);
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

    // Initialize Shared DSP Config
    shared_config.tempo = 1.0f;
    shared_config.pitch = 1.0f;
    pthread_mutex_init(&shared_config.lock, NULL);

    // Opens WAV file and extracts structural metadata
    SF_INFO sfinfo;
    sfinfo.format = 0;
    infile = sf_open(argv[1], SFM_READ, &sfinfo);
    if (!infile) return EXIT_FAILURE;

    // Initialize and configure the ALSA PCM Hardware Interface
    if (snd_pcm_open(&pcm_handle, PCM_DEVICE, SND_PCM_STREAM_PLAYBACK, 0) < 0) return EXIT_FAILURE;
    snd_pcm_set_params(pcm_handle, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, sfinfo.channels, sfinfo.samplerate, 1, 500000);

    // Allocate memory and initialize synchronization for the Circular Buffer
    ring_buf.max_frames = CHUNK_FRAMES * RING_BUFFER_CHUNKS;
    ring_buf.channels = sfinfo.channels;
    ring_buf.data = malloc(ring_buf.max_frames * ring_buf.channels * sizeof(short));
    ring_buf.head = ring_buf.tail = ring_buf.count = 0;
    ring_buf.eof_reached = false;
    
    pthread_mutex_init(&ring_buf.lock, NULL);
    pthread_cond_init(&ring_buf.not_empty, NULL);
    pthread_cond_init(&ring_buf.not_full, NULL);

    printf("--- Starting Multithreaded Audio Engine ---\n");

    // Release concurrent execution threads
    pthread_t producer, consumer, control;
    pthread_create(&producer, NULL, producer_thread_func, NULL);
    pthread_create(&consumer, NULL, consumer_thread_func, NULL);
    pthread_create(&control, NULL, control_thread_func, NULL);

    // Blocks main execution until the primary operations threads temrinate
    pthread_join(producer, NULL);
    pthread_join(consumer, NULL);

    // Note: The control thread is a daemon thread in this design.
    // It will cleanly die when the main process exits after playback finishes.

    // Execute graceful shutdown of hardware peripherals, memory, and synchronization variables
    snd_pcm_drain(pcm_handle);
    snd_pcm_close(pcm_handle);
    sf_close(infile);
    free(ring_buf.data);
    pthread_mutex_destroy(&ring_buf.lock);
    pthread_cond_destroy(&ring_buf.not_empty);
    pthread_cond_destroy(&ring_buf.not_full);
    pthread_mutex_destroy(&shared_config.lock);

    printf("--- System Shut Down Cleanly ---\n");
    return EXIT_SUCCESS;
}
