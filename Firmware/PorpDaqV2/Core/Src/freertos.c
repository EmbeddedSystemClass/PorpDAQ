#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "cmsis_os.h"
#include "fatfs.h"
#include "utils.h"
#include "adc.h"
#include "arm_math.h"
#include "arm_const_structs.h"
#include "gpio.h"
#include "usart.h"
#include "wav.h"
#include "time.h"

// Size of mail queue
#define MAIL_SIZE (uint32_t)6
// Number of samples to be written to SD Card
#define SD_WRITE_BUF_SIZE 16384
/* Size of array containing ADC converted values */
#define ADC_BUFFER_LENGTH ((uint32_t) 16384)

/*-------- FFT Parameters----*/
#define FFT_BUFFER_LENGTH 2048  // RFFT buffer size
#define FFT_SIZE 1024
#define FFT_INVERSE_FLAG        ((uint8_t)0)
#define FFT_Normal_OUTPUT_FLAG  ((uint8_t)1)
float32_t freq_component;
arm_rfft_fast_instance_f32 rfft_instance;
arm_status fft_status;
float32_t fft_input[FFT_SIZE];
float32_t fft_output[FFT_SIZE];
float32_t mag_array[FFT_SIZE];
uint32_t maxIndex = 0;
float32_t maxValue = 0;


// debug to check if mail queue is full.
int mail_retval;

// ADC Struct
typedef struct
{
  uint16_t data[8192];
}ADC_DATA;

ADC_DATA *prod_data;
uint16_t sd_buff[8192]; // Array to hold data to be written to the SD Card
uint16_t tmp_array[FFT_SIZE];// temporarily calculate FFT of each 1024 points with this array


ALIGN_32BYTES (static uint16_t   adc_values[ADC_BUFFER_LENGTH]); // Array to hold data read from ADC
uint8_t uart_tx_buffer[UART_TX_BUFFER_SIZE]; // Stores FFT output to be transmitted to the Pi


/*----WAV file parameters----*/
int waypoint_index = 0;
char WAV_FILE[8];
FIL wavFile; // File Instance
uint32_t bytes_written = 0;
uint32_t file_size;
uint8_t wavHeaderBuff[44];
WAV_Format WaveFormat;
FATFS FatFSInstance;
uint8_t open_result; // returns true if fopen is successful
uint8_t wav_write_result; // returns true if f_write was succsessful


osThreadId prodTaskHandle;
osThreadId conTaskHandle;

osMailQId adcDataMailId;
osEvent prodEvent;
osEvent consEvent;
osEvent writeEvent;

uint8_t producer_signal = 0x01;  // signal for the producer task //

void vProducer(void const * argument);
void vConsumer(void const * argument);
void startADC(void);
void mountSDCard(void);
void stopADC(void);
void setFileName(void);
void writeSDCard(void);
void computeFFT(void);


extern void MX_FATFS_Init(void);
void MX_FREERTOS_Init(void); /* (MISRA C 2004 rule 8.1) */

/* GetIdleTaskMemory prototype (linked to static allocation support) */
void vApplicationGetIdleTaskMemory( StaticTask_t **ppxIdleTaskTCBBuffer, StackType_t **ppxIdleTaskStackBuffer, uint32_t *pulIdleTaskStackSize );

/* USER CODE BEGIN GET_IDLE_TASK_MEMORY */
static StaticTask_t xIdleTaskTCBBuffer;
static StackType_t xIdleStack[configMINIMAL_STACK_SIZE];

void vApplicationGetIdleTaskMemory( StaticTask_t **ppxIdleTaskTCBBuffer, StackType_t **ppxIdleTaskStackBuffer, uint32_t *pulIdleTaskStackSize )
{
  *ppxIdleTaskTCBBuffer = &xIdleTaskTCBBuffer;
  *ppxIdleTaskStackBuffer = &xIdleStack[0];
  *pulIdleTaskStackSize = configMINIMAL_STACK_SIZE;
}

/**
 * @brief  FreeRTOS initialization
 * @param  None
 * @retval None
 */
void MX_FREERTOS_Init(void)
{
  osThreadDef(prod_task, vProducer, osPriorityHigh, 0, 9216 );
  prodTaskHandle = osThreadCreate(osThread(prod_task), NULL);

  osThreadDef(con_task, vConsumer, osPriorityHigh, 0, 10752 );
  conTaskHandle = osThreadCreate(osThread(con_task), NULL);

  osMailQDef(mail, MAIL_SIZE, ADC_DATA);
  adcDataMailId = osMailCreate(osMailQ(mail), NULL);
}

/**
 * @brief Mounts SD Card
 * @retval Nothing
 */
void mountSDCard(void)
{
  if(f_mount(&FatFSInstance, SDPath, 1)==FR_OK)
  {
    // Turn on LED if mount was successful
    LED_ON();
    if(f_open(&wavFile, WAV_FILE, FA_WRITE|FA_CREATE_ALWAYS) == FR_OK)
    {
      // Initialise the WAV Header
      InitialiseWavEncoder(AUDIO_FREQ, wavHeaderBuff, &WaveFormat);
      // Write header file
      wav_write_result = f_write(&wavFile, wavHeaderBuff, 44,(void*)&bytes_written);
      // return size of file
      file_size = bytes_written;
    }
  }
}

/**
 * @brief sets filename for each
 * WAV file on each UART RX callback
 * @retval nothing
 */
void setFileName(void)
{
  // use waypoint index to write file name here
  waypoint_index++;
  snprintf(WAV_FILE, sizeof(WAV_FILE), "%d.wav", waypoint_index);
}

/**
 * Starts the ADC
 * @retval nothing
 */
void startADC(void)
{
  if(HAL_ADC_Start_DMA(&hadc3, (uint32_t*)&adc_values, ADC_BUFFER_LENGTH) != HAL_OK)
  {
  }
}

/**
 * @brief Stops the ADC
 */
void stopADC(void)
{
  if(HAL_ADC_Stop_DMA(&hadc3) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
 * @brief Producer Thread
 * Starts the ADC and mounts
 * the SDCard
 */
void vProducer(void const * argument)
{
  for(;;)
  {
    prodEvent = osSignalWait(0x01, osWaitForever);
    if(prodEvent.status == osEventSignal)
    {
      startADC();
      mountSDCard();
      osThreadSuspend(NULL);
    }
  }
}



/**
 * @brief Consumer Thread
 * Writes the data from the
 * ADC to the SD Card. Also
 * runs the FFT compute
 * function
 * @retval None
 */
void vConsumer(void const * argument)
{
  ADC_DATA *rx_data;
  for(;;)
  {
    // Wait for signal to start writing data
    writeEvent = osMailGet(adcDataMailId, osWaitForever);
    if(writeEvent.status == osEventMail)
    {
      // write Data to SD card
      rx_data =  writeEvent.value.p;
      memcpy(sd_buff, rx_data->data, sizeof(sd_buff));
      if(wav_write_result == FR_OK)
      {
        if( f_write(&wavFile, (uint8_t*)sd_buff, sizeof(sd_buff), (void*)&bytes_written) == FR_OK)
        {
          file_size+=bytes_written;
        }
      }
      //  computeFFT();
      // Release datra from queue after writing
      osMailFree(adcDataMailId, rx_data);
    }

    // Wait for signal to stop recording dara
    consEvent = osSignalWait(0x02, 0);
    if(consEvent.status == osEventSignal)
    {
      stopADC();
      if(f_lseek(&wavFile, 0) == FR_OK)
      {
        WavHeaderUpdate(wavHeaderBuff, &WaveFormat, file_size);
        // Update wav File
        if(f_write(&wavFile, wavHeaderBuff, sizeof(WaveFormat), (void*)&bytes_written)==FR_OK)
        {
          //close file
          f_close(&wavFile);
          // Turn off the LED
          LED_OFF();
          // release to Producer thread
          osThreadResume(prodTaskHandle);
        }
      }
    }
  }
}

/**
 * @brief computes a real FFT of
 * the incoming signals which is sent
 * over UART.
 */
void computeFFT(void)
{
  // Initialise Real FFT Instance
  arm_rfft_fast_init_f32(&rfft_instance, FFT_SIZE);
  // chunk current buffer into each  FFT size
  for(int j = 0; j < ADC_BUFFER_LENGTH/FFT_SIZE; j++)
  {
    // Copy data based on FFT Position from SD card buffer
    memcpy(tmp_array, &sd_buff[FFT_SIZE *j], sizeof(fft_input));

    // convert to floating point numbers
    for(int i = 0; i < FFT_SIZE; i++)
    {
      fft_input[i] = tmp_array[(uint16_t)i]/ (float32_t) 4096.0;
    }

    arm_rfft_fast_f32(&rfft_instance, fft_input, fft_output, FFT_INVERSE_FLAG);
    arm_cmplx_mag_f32(fft_output+2, mag_array+1, FFT_SIZE/2 -1);

    mag_array[0] = fft_output[0];
    mag_array[FFT_SIZE/2] = fft_output[1];
    mag_array[0] = 0;
    mag_array[FFT_SIZE/2] = 0;
    arm_max_f32(mag_array, FFT_SIZE, &maxValue, &maxIndex);

    // Find the dominant frequency component
    float32_t freq_multiplier = AUDIO_FREQ / FFT_SIZE;
    freq_component = (maxIndex ) * freq_multiplier;
    // Only send data if its within the highpass filter settings we're looking
    // for
    if(freq_component > 90000 && freq_component < 150000 && maxValue > 1)
    {
      memcpy(uart_tx_buffer, &mag_array, FFT_BUFFER_LENGTH);
      // Append Tail of UART Buffer
      uart_tx_buffer[2048] = 's';
      uart_tx_buffer[2049] = 't';
      uart_tx_buffer[2050] = 'o';
      uart_tx_buffer[2051] = 'p';

      // Transmit Data to raspberry pi
      if(HAL_UART_Transmit_DMA(&huart8, uart_tx_buffer, UART_TX_BUFFER_SIZE) != HAL_OK)
      {
        Error_Handler();
      }
    }
  }
}





void HAL_ADC_ConvHalfCpltCallback(ADC_HandleTypeDef *AdcHandle)
{

  // Invalidate Cache address. Just casual H7 things
  SCB_InvalidateDCache_by_Addr((uint32_t*)&adc_values[0], ADC_BUFFER_LENGTH);
  prod_data 	=  osMailAlloc(adcDataMailId, osWaitForever);
  memcpy(prod_data->data, adc_values, sizeof(adc_values)/2);
  mail_retval = osMailPut(adcDataMailId, prod_data);
  if( mail_retval != osOK)
  {
    // Handle Error here
  }



}

void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef* hadc)
{


  SCB_InvalidateDCache_by_Addr((uint32_t *) &adc_values[ADC_BUFFER_LENGTH/2], ADC_BUFFER_LENGTH);
  prod_data 	=  osMailAlloc(adcDataMailId, osWaitForever);
  memcpy(prod_data->data, adc_values + ADC_BUFFER_LENGTH/2, sizeof(adc_values)/2);
  mail_retval = osMailPut(adcDataMailId, prod_data);
  if( mail_retval != osOK)
  {
    // Handle Error here
  }
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  if(huart->Instance == UART8)
  {
    // Wait for data from the pi.
    if(uart_rx_buffer == 0x31)
    {
      setFileName();
      osSignalSet(prodTaskHandle, 0x01);
    }
    if(uart_rx_buffer == 0x32)
    {
      osSignalSet(conTaskHandle, 0x02);
    }
  }
}

