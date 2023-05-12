//-----------------------------------------------------------------------------
// Project:      CETI Tag Electronics
// Version:      Refer to _versioning.h
// Copyright:    Cummings Electronics Labs, Harvard University Wood Lab, MIT CSAIL
// Contributors: Joseph DelPreto [TODO: Add other contributors here]
//-----------------------------------------------------------------------------

#include "ecg.h"

//-----------------------------------------------------------------------------
// Initialization
//-----------------------------------------------------------------------------

// Global/static variables
int g_ecg_thread_getData_is_running = 0;
int g_ecg_thread_writeData_is_running = 0;
static char ecg_data_filepath[100];
static FILE* ecg_data_file = NULL;
static const char* ecg_data_file_headers[] = {
  "Sample Index",
  "ECG",
  "Leads-Off",
  };
static const int num_ecg_data_file_headers = 3;

static int ecg_buffer_select_toLog = 0;   // which buffer will be populated with new incoming data
static int ecg_buffer_select_toWrite = 0; // which buffer will be flushed to the output file
static int ecg_buffer_index_toLog = 0;
static long long global_times_us[ECG_NUM_BUFFERS][ECG_BUFFER_LENGTH] = {0};
static int rtc_counts[ECG_NUM_BUFFERS][ECG_BUFFER_LENGTH] = {0};
static long ecg_readings[ECG_NUM_BUFFERS][ECG_BUFFER_LENGTH] = {0};
static int leadsOff_readings[ECG_NUM_BUFFERS][ECG_BUFFER_LENGTH] = {0};
static long long sample_indexes[ECG_NUM_BUFFERS][ECG_BUFFER_LENGTH] = {0};
static char ecg_data_file_notes[ECG_NUM_BUFFERS][ECG_BUFFER_LENGTH][25];

int init_ecg() {
  // Initialize the GPIO expander and the ADC.
  init_ecg_electronics();

  // Open an output file to write data.
  if(init_ecg_data_file(1) < 0)
    return -1;

  return 0;
}
int init_ecg_electronics() {

  // Set up the GPIO expander.
  //   The ADC code will use it to poll the data-ready output,
  //   and this main loop will use it to read the ECG leads-off detection output.
  if(ecg_gpio_expander_setup(ECG_I2C_BUS) < 0)
    return -1;

  // Set up and configure the ADC.
  if(ecg_adc_setup(ECG_I2C_BUS) < 0)
    return -1;
  ecg_adc_set_voltage_reference(ECG_ADC_VREF_EXTERNAL); // ECG_ADC_VREF_EXTERNAL or ECG_ADC_VREF_INTERNAL
  ecg_adc_set_gain(ECG_ADC_GAIN_ONE); // ECG_ADC_GAIN_ONE or ECG_ADC_GAIN_FOUR
  ecg_adc_set_data_rate(1000); // 20, 90, 330, or 1000
  ecg_adc_set_conversion_mode(ECG_ADC_MODE_CONTINUOUS); // ECG_ADC_MODE_CONTINUOUS or ECG_ADC_MODE_SINGLE_SHOT
  ecg_adc_set_channel(ECG_ADC_CHANNEL_ECG);
  // Start continuous conversion (or a single reading).
  ecg_adc_start();

  CETI_LOG("init_ecg(): Successfully initialized the ECG electronics");

  return 0;
}

//-----------------------------------------------------------------------------
// Helpers
//-----------------------------------------------------------------------------

// Determine a new ECG data filename that does not already exist.
int init_ecg_data_file(int restarted_program)
{
  // Append a number to the filename base until one is found that doesn't exist yet.
  int data_file_postfix_count = 0;
  int data_file_exists = 0;
  do
  {
    sprintf(ecg_data_filepath, "%s_%02d.csv", ECG_DATA_FILEPATH_BASE, data_file_postfix_count);
    data_file_exists = (access(ecg_data_filepath, F_OK) != -1);
    data_file_postfix_count++;
  } while(data_file_exists);

  // Open the new file.
  int init_data_file_success = init_data_file(ecg_data_file, ecg_data_filepath,
                                              ecg_data_file_headers,  num_ecg_data_file_headers,
                                              ecg_data_file_notes[ecg_buffer_select_toLog][0],
                                              "init_ecg_data_file()");
  // Change the note from restarted to new file if this is not the first initialization.
  if(!restarted_program)
    strcpy(ecg_data_file_notes[ecg_buffer_select_toLog][0], "New log file! | ");
  ecg_data_file = fopen(ecg_data_filepath, "at");
  return init_data_file_success;
}

//-----------------------------------------------------------------------------
// Thread to acquire data into a rolling buffer
//-----------------------------------------------------------------------------
void* ecg_thread_getData(void* paramPtr)
{
  // Get the thread ID, so the system monitor can check its CPU assignment.
  g_ecg_thread_getData_tid = gettid();

  // Set the thread CPU affinity.
  if(ECG_GETDATA_CPU >= 0)
  {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(ECG_GETDATA_CPU, &cpuset);
    if(pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) == 0)
      CETI_LOG("ecg_thread_getData(): Successfully set affinity to CPU %d", ECG_GETDATA_CPU);
    else
      CETI_LOG("ecg_thread_getData(): XXX Failed to set affinity to CPU %d", ECG_GETDATA_CPU);
  }
  // Set the thread priority.
  struct sched_param sp;
  memset(&sp, 0, sizeof(sp));
  sp.sched_priority = sched_get_priority_max(SCHED_RR);
  if(pthread_setschedparam(pthread_self(), SCHED_RR, &sp) == 0)
    CETI_LOG("ecg_thread_getData(): Successfully set priority");
  else
    CETI_LOG("ecg_thread_getData(): XXX Failed to set priority");

  // Main loop while application is running.
  CETI_LOG("ecg_thread_getData(): Starting loop to periodically acquire data");
  g_ecg_thread_getData_is_running = 1;

  // Continuously poll the ADC and the leads-off detection output.
  long long prev_ecg_adc_latest_reading_global_time_us = 0;
  ecg_buffer_index_toLog = 0;
  long long sample_index = 0;
  long long start_time_ms = get_global_time_ms();
  while(!g_exit)
  {
    // Request an update of the ECG data, then see if new data was received yet.
    //  The new data may be read immediately by this call after waiting for data to be ready,
    //  or nothing may happen if waiting for an interrupt callback to be triggered.
    ecg_adc_update_data(&g_exit, ECG_DATAREADY_TIMEOUT_US);
    if(g_ecg_adc_latest_reading_global_time_us != prev_ecg_adc_latest_reading_global_time_us)
    {
      // Store the new data sample and its timestamp.
      ecg_readings[ecg_buffer_select_toLog][ecg_buffer_index_toLog] = g_ecg_adc_latest_reading;
      global_times_us[ecg_buffer_select_toLog][ecg_buffer_index_toLog] = g_ecg_adc_latest_reading_global_time_us;
      // Update the previous timestamp, for checking whether new data is available.
      prev_ecg_adc_latest_reading_global_time_us = global_times_us[ecg_buffer_select_toLog][ecg_buffer_index_toLog];

      // Read the GPIO expander for the latest leads-off detection.
      // Assume it's fast enough that the ECG sample timestamp is close enough to this leads-off timestamp.
      leadsOff_readings[ecg_buffer_select_toLog][ecg_buffer_index_toLog]
        = ecg_gpio_expander_read_leadsOff();

      // Read the RTC.
      rtc_counts[ecg_buffer_select_toLog][ecg_buffer_index_toLog] = getRtcCount();

      // Update indexes.
      sample_indexes[ecg_buffer_select_toLog][ecg_buffer_index_toLog] = sample_index;
      sample_index++;

      //    CETI_LOG("ADC Reading! %ld\n", ecg_readings[ecg_buffer_index_toLog]);
      //    CETI_LOG("ADC Reading! %6.3f ", 3.3*(float)ecg_readings[ecg_buffer_index_toLog]/(float)(1 << 23));
      //    CETI_LOG("\tLeadsOff Reading! %1d\n", leadsOff_readings[ecg_buffer_index_toLog]);

      // If there was an error reading, delay a bit before retrying.
      if(ecg_readings[ecg_buffer_select_toLog][ecg_buffer_index_toLog] == ECG_INVALID_PLACEHOLDER)
      {
        strcat(ecg_data_file_notes[ecg_buffer_select_toLog][ecg_buffer_index_toLog], "INVALID? | ");
        usleep(1000000);
        init_ecg_electronics();
        usleep(10000);
      }

      // Advance the buffer index.
      // If the buffer has filled, switch to the other buffer
      //   (this will also trigger the writeData thread to write the previous buffer to a file).
      ecg_buffer_index_toLog++;
      if(ecg_buffer_index_toLog == ECG_BUFFER_LENGTH)
      {
        ecg_buffer_index_toLog = 0;
        ecg_buffer_select_toLog++;
        ecg_buffer_select_toLog %= ECG_NUM_BUFFERS;
      }

      // Clear the next notes.
      strcpy(ecg_data_file_notes[ecg_buffer_select_toLog][ecg_buffer_index_toLog], "");
    }

    // Note that there is no delay to implement a desired sampling rate,
    //  since the rate will be set by the ADC configuration.
  }

  // Print the duration and the sampling rate.
  long long duration_ms = get_global_time_ms() - start_time_ms;
  CETI_LOG("ecg_thread_getData(): Average rate %0.2f Hz (%lld samples in %lld ms)",
            1000.0*(float)sample_index/(float)duration_ms,
            sample_index, duration_ms);

  // Clean up.
  ecg_adc_cleanup();
  ecg_gpio_expander_cleanup();

  g_ecg_thread_getData_is_running = 0;
  CETI_LOG("ecg_thread_getData(): Done!");
  return NULL;
}

//-----------------------------------------------------------------------------
// Thread to write data from the rolling buffer to a file
//-----------------------------------------------------------------------------
void* ecg_thread_writeData(void* paramPtr)
{
  // Get the thread ID, so the system monitor can check its CPU assignment.
  g_ecg_thread_writeData_tid = gettid();

  // Set the thread CPU affinity.
  if(ECG_WRITEDATA_CPU >= 0)
  {
    pthread_t thread;
    thread = pthread_self();
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(ECG_WRITEDATA_CPU, &cpuset);
    if(pthread_setaffinity_np(thread, sizeof(cpuset), &cpuset) == 0)
      CETI_LOG("ecg_thread_writeData(): Successfully set affinity to CPU %d", ECG_WRITEDATA_CPU);
    else
      CETI_LOG("ecg_thread_writeData(): XXX Failed to set affinity to CPU %d", ECG_WRITEDATA_CPU);
  }

  // Main loop while application is running.
  CETI_LOG("ecg_thread_writeData(): Starting loop to write data as it is acquired");
  g_ecg_thread_writeData_is_running = 1;

  // Continuously wait for new data and then write it to the file.
  while(!g_exit)
  {
    // Wait for new data to be in the buffer.
    while(ecg_buffer_select_toLog == ecg_buffer_select_toWrite && !g_exit)
      usleep(100000);

    // Write the last buffer to a file.
    long ecg_data_file_size_b = 0;
    ecg_data_file = fopen(ecg_data_filepath, "at");
    if(ecg_data_file == NULL)
    {
      CETI_LOG("ecg_thread_writeData(): failed to open data output file: %s", ecg_data_filepath);
      init_ecg_data_file(0);
    }
    else
    {
      // Determine the last index to write.
      // During normal operation, will want to write the entire buffer
      //  since the acquisition thread has just finished filling it.
      int ecg_buffer_last_index_toWrite = ECG_BUFFER_LENGTH-1;
      // If the program exited though, will want to write only as much
      //  as the acquisition thread has filled.
      if(ecg_buffer_select_toLog == ecg_buffer_select_toWrite)
      {
        ecg_buffer_last_index_toWrite = ecg_buffer_index_toLog-1;
        if(ecg_buffer_last_index_toWrite < 0)
          ecg_buffer_last_index_toWrite = 0;
      }
      // Write the buffer data to the file.
      for(int ecg_buffer_index_toWrite = 0; ecg_buffer_index_toWrite <= ecg_buffer_last_index_toWrite; ecg_buffer_index_toWrite++)
      {
        // Write timing information.
        fprintf(ecg_data_file, "%lld", global_times_us[ecg_buffer_select_toWrite][ecg_buffer_index_toWrite]);
        fprintf(ecg_data_file, ",%d", rtc_counts[ecg_buffer_select_toWrite][ecg_buffer_index_toWrite]);
        // Write any notes.
        fprintf(ecg_data_file, ",%s", ecg_data_file_notes[ecg_buffer_select_toWrite][ecg_buffer_index_toWrite]);
        // Write the sensor data.
        fprintf(ecg_data_file, ",%lld", sample_indexes[ecg_buffer_select_toWrite][ecg_buffer_index_toWrite]);
        fprintf(ecg_data_file, ",%ld", ecg_readings[ecg_buffer_select_toWrite][ecg_buffer_index_toWrite]);
        fprintf(ecg_data_file, ",%d", leadsOff_readings[ecg_buffer_select_toWrite][ecg_buffer_index_toWrite]);
        // Finish the row of data.
        fprintf(ecg_data_file, "\n");
      }
      // Check the file size and close the file.
      fseek(ecg_data_file, 0L, SEEK_END);
      ecg_data_file_size_b = ftell(ecg_data_file);
      fclose(ecg_data_file);

      // If the file size limit has been reached, start a new file.
      if((ecg_data_file_size_b >= (long)(ECG_MAX_FILE_SIZE_MB)*1024L*1024L || ecg_data_file_size_b < 0) && !g_exit)
        init_ecg_data_file(0);

      //CETI_LOG("Wrote %d entries in %lld us", ECG_BUFFER_LENGTH, get_global_time_us() - start_time_us);
    }
    // Advance to the next buffer.
    ecg_buffer_select_toWrite++;
    ecg_buffer_select_toWrite %= ECG_NUM_BUFFERS;
  }

  // Clean up.
  g_ecg_thread_writeData_is_running = 0;
  CETI_LOG("ecg_thread_writeData(): Done!");
  return NULL;
}





