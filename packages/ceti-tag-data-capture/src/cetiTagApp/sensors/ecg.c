
//-----------------------------------------------------------------------------
// Project:      CETI Tag Electronics
// Version:      Refer to _versioning.h
// Copyright:    Cummings Electronics Labs, Harvard University Wood Lab, MIT CSAIL
// Contributors: Joseph DelPreto [TODO: Add other contributors here]
//-----------------------------------------------------------------------------

#include "ecg.h"
#include "ecg_helpers/ecg_lod.h"

#if ECG_OLD
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
  "Leads-Off-P",
  "Leads-Off-N",
  };
static const int num_ecg_data_file_headers = 4;

static int ecg_buffer_select_toLog = 0;   // which buffer will be populated with new incoming data
static int ecg_buffer_select_toWrite = 0; // which buffer will be flushed to the output file
static int ecg_buffer_index_toLog = 0;
static long long global_times_us[ECG_NUM_BUFFERS][ECG_BUFFER_LENGTH] = {0};
static int rtc_counts[ECG_NUM_BUFFERS][ECG_BUFFER_LENGTH] = {0};
static long ecg_readings[ECG_NUM_BUFFERS][ECG_BUFFER_LENGTH] = {0};
#if ECG_LOD_ENABLED
static int leadsOff_readings_p[ECG_NUM_BUFFERS][ECG_BUFFER_LENGTH] = {0};
static int leadsOff_readings_n[ECG_NUM_BUFFERS][ECG_BUFFER_LENGTH] = {0};
#endif
static long long sample_indexes[ECG_NUM_BUFFERS][ECG_BUFFER_LENGTH] = {0};
static char ecg_data_file_notes[ECG_NUM_BUFFERS][ECG_BUFFER_LENGTH][75];

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
  #if ECG_LOD_ENABLED
  WTResult lod_result = wt_ecg_iox_init();
  if(lod_result != WT_OK){
    CETI_ERR("%s", wt_strerror(lod_result));
    return -1;
  }
  #endif

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

  CETI_LOG("Successfully initialized the ECG electronics");
  CETI_LOG("ECG data-ready pin: %d", ECG_ADC_DATA_READY_PIN);

  return 0;
}

//-----------------------------------------------------------------------------
// Helpers
//-----------------------------------------------------------------------------

// Determine a new ECG data filename that does not already exist, and open a file for it.
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
      CETI_LOG("Successfully set affinity to CPU %d", ECG_GETDATA_CPU);
    else
      CETI_LOG("XXX Failed to set affinity to CPU %d", ECG_GETDATA_CPU);
  }
  // Set the thread priority.
  struct sched_param sp;
  memset(&sp, 0, sizeof(sp));
  sp.sched_priority = sched_get_priority_max(SCHED_RR);
  if(pthread_setschedparam(pthread_self(), SCHED_RR, &sp) == 0)
    CETI_LOG("Successfully set priority");
  else
    CETI_LOG("XXX Failed to set priority");

  // Main loop while application is running.
  CETI_LOG("Starting loop to periodically acquire data");
  g_ecg_thread_getData_is_running = 1;

  // Continuously poll the ADC and the leads-off detection output.
  long long prev_ecg_adc_latest_reading_global_time_us = 0;
  ecg_buffer_index_toLog = 0;
  long long sample_index = 0;
  long consecutive_zero_ecg_count = 0;
  long instantaneous_sampling_period_us = 0;
  int first_sample = 1;
  int is_invalid = 0;
  long long start_time_ms = get_global_time_ms();
  int previous_leadsoff = 0;
  while(!g_stopAcquisition)
  {
    // Request an update of the ECG data, then see if new data was received yet.
    //  The new data may be read immediately by this call after waiting for data to be ready,
    //  or nothing may happen if waiting for an interrupt callback to be triggered.
    ecg_adc_update_data(&g_stopAcquisition, ECG_SAMPLE_TIMEOUT_US);
    if(g_ecg_adc_latest_reading_global_time_us != prev_ecg_adc_latest_reading_global_time_us)
    {
      // Store the new data sample and its timestamp.
      ecg_readings[ecg_buffer_select_toLog][ecg_buffer_index_toLog] = g_ecg_adc_latest_reading;
      global_times_us[ecg_buffer_select_toLog][ecg_buffer_index_toLog] = g_ecg_adc_latest_reading_global_time_us;
      // Update the previous timestamp, for checking whether new data is available.
      instantaneous_sampling_period_us = global_times_us[ecg_buffer_select_toLog][ecg_buffer_index_toLog] - prev_ecg_adc_latest_reading_global_time_us;
      prev_ecg_adc_latest_reading_global_time_us = global_times_us[ecg_buffer_select_toLog][ecg_buffer_index_toLog];

      #if ECG_LOD_ENABLED
      // Read the GPIO expander for the latest leads-off detection.
      // Assume it's fast enough that the ECG sample timestamp is close enough to this leads-off timestamp.
      wt_ecg_iox_read_leadsOff_p(&leadsOff_readings_p[ecg_buffer_select_toLog][ecg_buffer_index_toLog]);
      wt_ecg_iox_read_leadsOff_n(leadsOff_readings_n[ecg_buffer_select_toLog][ecg_buffer_index_toLog]);
      #endif
      
      // Read the RTC.
      rtc_counts[ecg_buffer_select_toLog][ecg_buffer_index_toLog] = getRtcCount();

      // Update indexes.
      sample_indexes[ecg_buffer_select_toLog][ecg_buffer_index_toLog] = sample_index;
      sample_index++;

      // Check if there was an error reading from the ADC.
      // Note that the sample will already be set to ECG_INVALID_PLACEHOLDER
      //  if there was an explicit I2C error communicating with the ADC.
      // But if the ECG board is not connected, then the ADC will seemingly
      //  always have data ready and always return 0.
      // So also check if the ADC returned exactly 0 many times in a row.
      if(ecg_readings[ecg_buffer_select_toLog][ecg_buffer_index_toLog] == ECG_INVALID_PLACEHOLDER)
      {
        is_invalid = 1;
        strcat(ecg_data_file_notes[ecg_buffer_select_toLog][ecg_buffer_index_toLog], "ADC ERROR | ");
        CETI_DEBUG("XXX ADC encountered an error");
      }
      if(ecg_readings[ecg_buffer_select_toLog][ecg_buffer_index_toLog] == 0)
        consecutive_zero_ecg_count++;
      else
        consecutive_zero_ecg_count = 0;
      if(consecutive_zero_ecg_count > ECG_ZEROCOUNT_THRESHOLD)
      {
        is_invalid = 1;
        strcat(ecg_data_file_notes[ecg_buffer_select_toLog][ecg_buffer_index_toLog], "ADC ZEROS | ");
        CETI_DEBUG("ADC returned %ld zero readings in a row", consecutive_zero_ecg_count);
      }

      #if ECG_LOD_ENABLED
      // Check if there was an error communicating with the GPIO expander.
      if(leadsOff_readings_p[ecg_buffer_select_toLog][ecg_buffer_index_toLog] == ECG_LEADSOFF_INVALID_PLACEHOLDER
         || leadsOff_readings_n[ecg_buffer_select_toLog][ecg_buffer_index_toLog] == ECG_LEADSOFF_INVALID_PLACEHOLDER)
      {
        is_invalid = 1;
        strcat(ecg_data_file_notes[ecg_buffer_select_toLog][ecg_buffer_index_toLog], "LO ERROR | ");
        CETI_LOG("XXX The GPIO expander encountered an error");
      }
      #endif

      // Check if it took longer than expected to receive the sample (from the ADC and the GPIO expander combined).
      if(instantaneous_sampling_period_us > ECG_SAMPLE_TIMEOUT_US && !first_sample)
      {
        is_invalid = 1;
        strcat(ecg_data_file_notes[ecg_buffer_select_toLog][ecg_buffer_index_toLog], "TIMEOUT | ");
        CETI_DEBUG("XXX Reading a sample took %ld us", instantaneous_sampling_period_us);
      }
      first_sample = 0;
      // If the ADC or the GPIO expander had an error,
      //  wait a bit and then try to reconnect to them.
      if(is_invalid && !g_stopAcquisition)
      {
        strcat(ecg_data_file_notes[ecg_buffer_select_toLog][ecg_buffer_index_toLog], "INVALID? | ");
        usleep(1000000);
        init_ecg_electronics();
        usleep(10000);
        consecutive_zero_ecg_count = 0;
        first_sample = 1;
        is_invalid = 0;
        previous_leadsoff = 0;
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
  CETI_LOG("Average rate %0.2f Hz (%lld samples in %lld ms)",
            1000.0*(float)sample_index/(float)duration_ms,
            sample_index, duration_ms);

  // Clean up.
  ecg_adc_cleanup();

  g_ecg_thread_getData_is_running = 0;
  CETI_LOG("Done!");
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
      CETI_LOG("Successfully set affinity to CPU %d", ECG_WRITEDATA_CPU);
    else
      CETI_LOG("XXX Failed to set affinity to CPU %d", ECG_WRITEDATA_CPU);
  }
  // Set the thread to a high priority.
 struct sched_param sp;
 memset(&sp, 0, sizeof(sp));
 sp.sched_priority = sched_get_priority_max(SCHED_RR);
 if(pthread_setschedparam(pthread_self(), SCHED_RR, &sp) == 0)
   CETI_LOG("Successfully set priority");
 else
   CETI_LOG("XXX Failed to set priority");

  // Main loop while application is running.
  CETI_LOG("Starting loop to write data as it is acquired");
  g_ecg_thread_writeData_is_running = 1;

  // Continuously wait for new data and then write it to the file.
  while(!g_stopAcquisition)
  {
    // Wait for new data to be in the buffer.
    while(ecg_buffer_select_toLog == ecg_buffer_select_toWrite && !g_stopAcquisition)
      usleep(250000);

    // Write the last buffer to a file.
    long ecg_data_file_size_b = 0;
    ecg_data_file = fopen(ecg_data_filepath, "at");
    if(ecg_data_file == NULL)
    {
      CETI_LOG("failed to open data output file: %s", ecg_data_filepath);
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
        #if ECG_LOD_ENABLE
        fprintf(ecg_data_file, ",%d", leadsOff_readings_p[ecg_buffer_select_toWrite][ecg_buffer_index_toWrite]);
        fprintf(ecg_data_file, ",%d", leadsOff_readings_n[ecg_buffer_select_toWrite][ecg_buffer_index_toWrite]);
        #else
        fprintf(ecg_data_file, ", , ");
        #endif
        // Finish the row of data.
        fprintf(ecg_data_file, "\n");
      }
      // Check the file size and close the file.
      fseek(ecg_data_file, 0L, SEEK_END);
      ecg_data_file_size_b = ftell(ecg_data_file);
      fclose(ecg_data_file);

      // If the file size limit has been reached, start a new file.
      if((ecg_data_file_size_b >= (long)(ECG_MAX_FILE_SIZE_MB)*1024L*1024L || ecg_data_file_size_b < 0) && !g_stopAcquisition)
        init_ecg_data_file(0);

      //CETI_LOG("Wrote %d entries in %lld us", ECG_BUFFER_LENGTH, get_global_time_us() - start_time_us);
    }
    // Advance to the next buffer.
    ecg_buffer_select_toWrite++;
    ecg_buffer_select_toWrite %= ECG_NUM_BUFFERS;
  }

  // Clean up.
  g_ecg_thread_writeData_is_running = 0;
  CETI_LOG("Done!");
  return NULL;
}

#else

//-----------------------------------------------------------------------------
// Initialization
//-----------------------------------------------------------------------------

#define ECG_FLAG_ADC_ERROR (1 << 0)
#define ECG_FLAG_ADC_ZEROS (1 << 1)
#define ECG_FLAG_EXP_ERROR (1 << 2)
#define ECG_FLAG_TIMEOUT   (1 << 3)
#define ECG_FLAG_RESTARTED (1 << 4)
#define ECG_FLAG_NEW_LOG    (1 << 5)
#define ECG_ERROR_FLAG_MASK (ECG_FLAG_ADC_ERROR | ECG_FLAG_ADC_ZEROS | ECG_FLAG_EXP_ERROR | ECG_FLAG_TIMEOUT)

#define ECG_SLEEP_INTERVAL_uSECONDS 100

typedef struct ecg_sample_t{
  time_t   sys_clock;
  uint32_t rtc;
  uint32_t index;
  int32_t  value;
  uint8_t  flags;
  uint8_t  gpio_expander;
}ECGSample;

// Global/static variables
int g_ecg_thread_getData_is_running = 0;
int g_ecg_thread_writeData_is_running = 0;
static char ecg_data_filepath[100];
static FILE* ecg_data_file = NULL;
#if !(ECG_BINARY_WRITE)
static const char* ecg_data_file_headers[] = {
  "Sample Index",
  "ECG",
  "Leads-Off-P",
  "Leads-Off-N",
  };
static const int num_ecg_data_file_headers = 4;
#endif

static int ecg_buffer_select_toLog = 0;   // which buffer will be populated with new incoming data
static int ecg_buffer_select_toWrite = 0; // which buffer will be flushed to the output file
static uint_fast16_t ecg_buffer_index_toLog = 0;
static long ecg_data_file_size_b = 0;
static uint8_t ecg_restart_flag = ECG_FLAG_RESTARTED;
static ECGSample ecg_samples[ECG_NUM_BUFFERS][ECG_BUFFER_LENGTH];


int init_ecg() {
  // Initialize the GPIO expander and the ADC.
  init_ecg_electronics();
  ecg_restart_flag = ECG_FLAG_RESTARTED;
  // Open an output file to write data.
  if(init_ecg_data_file() < 0)
    return -1;

  return 0;
}

int init_ecg_electronics() {

  #if ECG_LOD_ENABLE
  // Set up the GPIO expander.
  //   The ADC code will use it to poll the data-ready output,
  //   and this main loop will use it to read the ECG leads-off detection output.
  WTResult lod_result = wt_ecg_iox_init();
  if(lod_result != WT_OK){
    CETI_ERR("%s", wt_strerror(lod_result));
    return -1;
  }
  #endif

  // Set up and configure the ADC.
  if(ecg_adc_setup(ECG_I2C_BUS) < 0)
    return -1;
  ecg_adc_set_voltage_reference(ECG_ADC_VREF_EXTERNAL); // ECG_ADC_VREF_EXTERNAL or ECG_ADC_VREF_INTERNAL
  ecg_adc_set_gain(ECG_ADC_GAIN_ONE); // ECG_ADC_GAIN_ONE or ECG_ADC_GAIN_FOUR
  ecg_adc_set_data_rate(1000); // 20, 90, 330, or 1000
  ecg_adc_set_conversion_mode(ECG_ADC_MODE_CONTINUOUS); // ECG_ADC_MODE_CONTINUOUS or ECG_ADC_MODE_SINGLE_SHOT
  ecg_adc_set_channel(ECG_ADC_CHANNEL_ECG);
  ecg_adc_config_apply();
  // Start continuous conversion (or a single reading).
  ecg_adc_start();

  CETI_LOG("Successfully initialized the ECG electronics");
  CETI_LOG("ECG data-ready pin: %d", ECG_ADC_DATA_READY_PIN);

  return 0;
}

//-----------------------------------------------------------------------------
// Helpers
//-----------------------------------------------------------------------------

// Determine a new ECG data filename that does not already exist, and open a file for it.
int init_ecg_data_file(void)
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
                                              NULL,
                                              "init_ecg_data_file()");
  ecg_restart_flag |= ECG_FLAG_NEW_LOG;

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
      CETI_LOG("Successfully set affinity to CPU %d", ECG_GETDATA_CPU);
    else
      CETI_ERR("Failed to set affinity to CPU %d", ECG_GETDATA_CPU);
  }
  // Set the thread priority.
  struct sched_param sp;
  memset(&sp, 0, sizeof(sp));
  sp.sched_priority = sched_get_priority_max(SCHED_RR);
  if(pthread_setschedparam(pthread_self(), SCHED_RR, &sp) == 0)
    CETI_LOG("Successfully set priority");
  else
    CETI_ERR("Failed to set priority");

  // Main loop while application is running.
  CETI_LOG("Starting loop to periodically acquire data");
  g_ecg_thread_getData_is_running = 1;

  // Continuously poll the ADC and the leads-off detection output.
  long long prev_ecg_adc_latest_reading_global_time_us = 0;
  ecg_buffer_index_toLog = 0;
  long long sample_index = 0;
  long consecutive_zero_ecg_count = 0;
  long instantaneous_sampling_period_us = 0;
  int first_sample = 1;
  long long start_time_ms = get_global_time_ms();
  int previous_leadsoff = 0;
  while(!g_stopAcquisition) {
    // Request an update of the ECG data, then see if new data was received yet.
    //  The new data may be read immediately by this call after waiting for data to be ready,
    //  or nothing may happen if waiting for an interrupt callback to be triggered.
    if(ecg_adc_read_data_ready() != 0){
      usleep(ECG_SLEEP_INTERVAL_uSECONDS);
      continue;
    }

    ECGSample *current_sample = &ecg_samples[ecg_buffer_select_toLog][ecg_buffer_index_toLog];
    *current_sample = (ECGSample){
      .value = ecg_adc_raw_read_data(),         // Store the new data sample and its timestamp.
      .sys_clock = get_global_time_us(),
      .rtc = getRtcCount(),                      //Read the RTC
      #if ECG_LOD_ENABLE
      #endif
      .index = sample_index++,                   // Update indexes.
      .flags = ecg_restart_flag,
    };
    iox_read_register(IOX_REG_INPUT, &current_sample->gpio_expander); //Read the GPIO expander for latest leads-off detection.
    ecg_restart_flag ^= ecg_restart_flag; //clear restart flag

    // Update the previous timestamp, for checking whether new data is available.
    instantaneous_sampling_period_us = current_sample->sys_clock - prev_ecg_adc_latest_reading_global_time_us;
    prev_ecg_adc_latest_reading_global_time_us = current_sample->sys_clock;

    // Check if there was an error reading from the ADC.
    // Note that the sample will already be set to ECG_INVALID_PLACEHOLDER
    //  if there was an explicit I2C error communicating with the ADC.
    // But if the ECG board is not connected, then the ADC will seemingly
    //  always have data ready and always return 0.
    // So also check if the ADC returned exactly 0 many times in a row.
    if(current_sample->value == ECG_INVALID_PLACEHOLDER) {
      current_sample->flags |= ECG_FLAG_ADC_ERROR;
      CETI_DEBUG("ADC encountered an error");
    }
    
    if(current_sample->value == 0){
      consecutive_zero_ecg_count++;
    } else {
      consecutive_zero_ecg_count = 0;
    }
    
    if(consecutive_zero_ecg_count > ECG_ZEROCOUNT_THRESHOLD){
      current_sample->flags |= ECG_FLAG_ADC_ZEROS;
      CETI_DEBUG("ADC returned %ld zero readings in a row", consecutive_zero_ecg_count);
    }
    // Check if there was an error communicating with the GPIO expander.
    #if ECG_LOD_ENABLE
    if( current_sample->gpio_expander == ECG_LEADSOFF_INVALID_PLACEHOLDER ){
      current_sample->flags |= ECG_FLAG_EXP_ERROR;
      CETI_DEBUG("The GPIO expander encountered an error");
    }
    #endif
    // Check if it took longer than expected to receive the sample (from the ADC and the GPIO expander combined).
    if(instantaneous_sampling_period_us > ECG_SAMPLE_TIMEOUT_US && !first_sample) {
      current_sample->flags |= ECG_FLAG_TIMEOUT;
      CETI_DEBUG("Reading a sample took %ld us", instantaneous_sampling_period_us);
    }
    first_sample = 0;
    // If the ADC or the GPIO expander had an error,
    //  wait a bit and then try to reconnect to them.
    if((current_sample->flags & ECG_ERROR_FLAG_MASK) && !g_exit) {
      usleep(1000000);
      init_ecg_electronics();
      usleep(10000);
      consecutive_zero_ecg_count = 0;
      first_sample = 1;
      previous_leadsoff = 0;
    }

    #if ECG_LOD_ENABLE
    // Set the LEDs to yellow if the leads are off.
    if( current_sample->gpio_expander & 0b00000011 ) { //ToDo: remove magic number
      previous_leadsoff = 1;
    } else {
      previous_leadsoff = 0;
    }
    #endif

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
    // Note that there is no delay to implement a desired sampling rate,
    //  since the rate will be set by the ADC configuration.
  }

  // Print the duration and the sampling rate.
  long long duration_ms = get_global_time_ms() - start_time_ms;
  CETI_LOG("Average rate %0.2f Hz (%lld samples in %lld ms)",
            1000.0*(float)sample_index/(float)duration_ms,
            sample_index, duration_ms);

  // Clean up.
  ecg_adc_cleanup();

  g_ecg_thread_getData_is_running = 0;
  CETI_LOG("Done!");
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
      CETI_LOG("Successfully set affinity to CPU %d", ECG_WRITEDATA_CPU);
    else
      CETI_ERR("Failed to set affinity to CPU %d", ECG_WRITEDATA_CPU);
  }
  // Set the thread to a high priority.
 struct sched_param sp;
 memset(&sp, 0, sizeof(sp));
 sp.sched_priority = sched_get_priority_max(SCHED_RR);
 if(pthread_setschedparam(pthread_self(), SCHED_RR, &sp) == 0)
   CETI_LOG("Successfully set priority");
 else
   CETI_ERR("Failed to set priority");

  // Main loop while application is running.
  CETI_LOG("Starting loop to write data as it is acquired");
  g_ecg_thread_writeData_is_running = 1;

  // Continuously wait for new data and then write it to the file.
  while(!g_stopAcquisition)
  {
    // Wait for new data to be in the buffer.
    while(ecg_buffer_select_toLog == ecg_buffer_select_toWrite && !g_exit)
      usleep(250000);

    // Write the last buffer to a file.
    ecg_data_file = fopen(ecg_data_filepath, "at");
    if(ecg_data_file == NULL){
      CETI_LOG("failed to open data output file: %s", ecg_data_filepath);
      init_ecg_data_file();
    } else {
      // Determine the last index to write.
      // During normal operation, will want to write the entire buffer
      //  since the acquisition thread has just finished filling it.
      int ecg_buffer_last_index_toWrite = ECG_BUFFER_LENGTH;
      // If the program exited though, will want to write only as much
      //  as the acquisition thread has filled.
      if(ecg_buffer_select_toLog == ecg_buffer_select_toWrite) {
        ecg_buffer_last_index_toWrite = ecg_buffer_index_toLog;
      }

      // Write the buffer data to the file.
      for(int i = 0; i < ecg_buffer_last_index_toWrite; i++){
        ECGSample *current_sample = &ecg_samples[ecg_buffer_select_toWrite][i]; 
        // Write timing information.
        fprintf(ecg_data_file, "%ld", current_sample->sys_clock);
        fprintf(ecg_data_file, ",%d", current_sample->rtc);
        // Write any notes.
        fprintf(ecg_data_file, ",");
        if(current_sample->flags & ECG_FLAG_RESTARTED)  { fprintf(ecg_data_file, "Restarted | ");     }
        if(current_sample->flags & ECG_FLAG_NEW_LOG)    { fprintf(ecg_data_file, "New log file! | "); }
        if(current_sample->flags & ECG_FLAG_ADC_ERROR)  { fprintf(ecg_data_file, "ADC ERROR | ");     }
        if(current_sample->flags & ECG_FLAG_ADC_ZEROS)  { fprintf(ecg_data_file, "ADC ZEROS | ");     }
        if(current_sample->flags & ECG_FLAG_EXP_ERROR)  { fprintf(ecg_data_file, "LO ERROR | ");      }
        if(current_sample->flags & ECG_FLAG_TIMEOUT)    { fprintf(ecg_data_file, "TIMEOUT | ");       }
        if(current_sample->flags & ECG_ERROR_FLAG_MASK) { fprintf(ecg_data_file, "INVALID? | ");      }
        // Write the sensor data.
        fprintf(ecg_data_file, ",%d", current_sample->index);
        fprintf(ecg_data_file, ",%d", current_sample->value);
        #if ECG_LOD_ENABLE
        fprintf(ecg_data_file, ",%d", wt_ecg_iox_parse_leadsOff_p(current_sample->gpio_expander));
        fprintf(ecg_data_file, ",%d", wt_ecg_iox_parse_leadsOff_n(current_sample->gpio_expander));
        #else
        fprintf(ecg_data_file, ", , ");
        #endif
        // Finish the row of data.
        fprintf(ecg_data_file, "\n");
      }
      // Check the file size and close the file.
      fseek(ecg_data_file, 0L, SEEK_END);
      ecg_data_file_size_b = ftell(ecg_data_file);

      /*CSV WRITE*/
      fclose(ecg_data_file);

      // If the file size limit has been reached, start a new file.
      if((ecg_data_file_size_b >= (long)(ECG_MAX_FILE_SIZE_MB)*1024L*1024L || ecg_data_file_size_b < 0) && !g_exit)
        init_ecg_data_file();

      //CETI_LOG("Wrote %d entries in %lld us", ECG_BUFFER_LENGTH, get_global_time_us() - start_time_us);
    }
    // Advance to the next buffer.
    ecg_buffer_select_toWrite++;
    ecg_buffer_select_toWrite %= ECG_NUM_BUFFERS;
  }

  // Clean up.
  g_ecg_thread_writeData_is_running = 0;
  CETI_LOG("Done!");
  return NULL;
}
#endif