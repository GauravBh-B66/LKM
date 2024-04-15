/*  
    This program creates a 16*2 LCD device driver.
*/

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/errno.h>
#include <linux/cdev.h>
#include <linux/gpio.h>
#include <linux/delay.h>

#define CLASS_NAME          "class_LCD"
#define DEVICE_NAME         "device_LCD"

MODULE_LICENSE ("GPL");
MODULE_AUTHOR ("Gaurav Bhattarai");
MODULE_DESCRIPTION ("16*2 LCD device driver.");
MODULE_VERSION ("One and Only");

static dev_t    deviceNumber;
static int      times = 0;

static struct class*    pClassLCD;
static struct device*   pDeviceLCD;
static struct cdev      cdevLCD;
static uint8_t lcdBuffer[17];   //16 characters + '\0'

uint8_t signalGpio[]={
    3,      //  Enable pin
    2,      //  Register select pin
    4,      //  Data Pin 0
	17,     //  Data Pin 1
	27,     //  Data Pin 2
	22,     //  Data Pin 3
	10,     //  Data Pin 4
	9,      //  Data Pin 5
	11,     //  Data Pin 6
	5      //  Data Pin 7 
};


#define ENABLE_PIN  signalGpio[0]

// REG_SEL_PIN = 0: Command mode
// REG_SEL_PIN = 1: Data mode
#define REG_SEL_PIN signalGpio[1]

#define CLEAR_SCREEN    0x01
#define DISPLAY_ON      0x0F        //Turns on the display; blinking cursor.

/*Bit mode configuration
    0|0|1|DL|N|F|-|-
        DL: Data length         [0: 4-bit; 1: 8-bit]
        N:  No of data lines    [0: 1-line; 1: 2-line]
        F:  Character Font      [0: 5*8 dots; 1: 5*10 dots]
*/
#define MODE_8_BIT      0X30    //8-bit; single line
#define MODE_4_BIT      0x20    //4-bit; single line


void enableHigh(void){
    gpio_set_value(ENABLE_PIN, 1);
    msleep(5);
    gpio_set_value(ENABLE_PIN, 0);
}

void sendCommand(uint8_t data){
    gpio_set_value(REG_SEL_PIN, 0);
    for (int i = 0; i <=7; i ++){
        gpio_set_value (signalGpio[i+2], (data & (1 << i)) >> i);
    }
    enableHigh();
}

void sendData(uint8_t data){
    gpio_set_value(REG_SEL_PIN, 1);
    for(int i=0; i<=7; i++){
        gpio_set_value (signalGpio[i+2], (data & (1 << i)) >> i);
    }
    enableHigh();
}

void setDisplay(void){
    sendCommand (MODE_8_BIT);
    sendCommand (DISPLAY_ON);
    sendCommand (CLEAR_SCREEN);
}

static int openDevice(struct inode* pInode, struct file* pFile){
    printk(KERN_INFO "LCD device file opened. Count = %d.\n", ++times);
    return 0;
}

static int releaseDevice(struct inode* pInode, struct file* pFile){
    printk(KERN_INFO "LCD device file closed/released.");
    return 0;
}

static ssize_t writeToDevice(struct file* pFile, const char* uBuffer, size_t requestedLength, loff_t* pOffset){
    int nError, nCopy;

    nCopy = min(requestedLength, sizeof(lcdBuffer));
    nError = copy_from_user(lcdBuffer, uBuffer, nCopy);

    sendCommand(CLEAR_SCREEN);
    for (int i=0; i < nCopy; i++){
        sendData(lcdBuffer[i]);
    }

    return (nCopy-nError);
}


static struct file_operations fops ={
    .owner = THIS_MODULE,
    .open = openDevice,
    .release = releaseDevice,
    .write = writeToDevice,
};


static int __init initFunction(void){
    //Out of 12 signal pins of LCD, Contrast Control and READ\WRITE are pins are not supported in this program.
    //Contrast control is controlled by an Analog Input (0-5V).
    //READ/WRITE wire is hardwired to be in Write Only Mode.
    int count;
    int nPins = 10;
    char *pinNames[] = {
        "LCD_ENABLE_PIN",
        "LCD_REGISTER_SELECT",
        "LCD_DATA_PIN0",
        "LCD_DATA_PIN1",
        "LCD_DATA_PIN2",
        "LCD_DATA_PIN3",
        "LCD_DATA_PIN4",
        "LCD_DATA_PIN5",
        "LCD_DATA_PIN6",
        "LCD_DATA_PIN7"
    };


    printk (KERN_INFO "Initializing the LCD device driver module.\n");
   
    //Allocate device number
    if ((alloc_chrdev_region(&deviceNumber, 12, 1, DEVICE_NAME)) < 0 ){
        printk("ERROR: Device number allocation failed.\n");
		return -1;
    }
    printk(KERN_INFO "SUCCESS: Device number allocation. Major:Minor=%d:%d", deviceNumber>>20, deviceNumber&0xfffff);

    //Create and register a class
    pClassLCD = class_create(THIS_MODULE, CLASS_NAME);
    if (IS_ERR(pClassLCD)){
        printk(KERN_INFO"ERROR %ld: Device class creation.\n", PTR_ERR(pClassLCD));
        goto errorClassRegistration;
    }
    printk (KERN_INFO "SUCCESS: Device class creation: %s.\n", CLASS_NAME);

    //Create device entry belonging to created class.
    pDeviceLCD = device_create(pClassLCD, NULL, deviceNumber, NULL, DEVICE_NAME);
    if (IS_ERR(pDeviceLCD)){
        printk(KERN_INFO"ERROR %ld: Device file creation.\n", PTR_ERR(pDeviceLCD));
        goto errorDeviceCreation;
    }

    //Attach the cdev structure with file operations allowed.
    cdev_init(&cdevLCD, &fops);
    //Attach the cdev structure to the device node created.
    if ((cdev_add(&cdevLCD, deviceNumber, 1)) < 0){
        printk(KERN_INFO"Failed during the registration of device to the kernel.");
        goto errorDeviceRegistration;
    }
    printk (KERN_INFO "Success: Device file registration.\n");

    //Request and set GPIOs to output mode.
    for (count = 0; count < nPins; count++){    
        if(gpio_request(signalGpio[count], pinNames[count]) < 0){
            printk("ERROR: Allocation of %s.\n", pinNames[count]);
            if (count == 0){
                goto errorDeviceRegistration;
            }
            count --;
            goto errorGPIOAllocation;
        };

        if (gpio_direction_output(signalGpio[count], 0) < 0){
            printk("ERROR: Setting the direction of%s.\n", pinNames[count]);
            goto errorSetDirectionOutput;
        }
    }

    return 0;


    errorSetDirectionOutput:
    errorGPIOAllocation:
        for(; count >=0; count --){
            gpio_free(signalGpio[count]);
        }
    errorDeviceRegistration:
        device_destroy(pClassLCD, deviceNumber);
    errorDeviceCreation:
        class_unregister(pClassLCD);
        class_destroy(pClassLCD);
    errorClassRegistration:
        unregister_chrdev_region(deviceNumber, 1);
    
    return -1;
}

static void __exit exitFunction(void){

    sendCommand(CLEAR_SCREEN);
    for (int i=0; i<10; i++){
        gpio_set_value(signalGpio[i], 0);
        gpio_free(signalGpio[i]);
    }

    cdev_del(&cdevLCD);
    device_destroy(pClassLCD, deviceNumber);
    class_destroy(pClassLCD);
    unregister_chrdev_region(deviceNumber, 1);

    printk(KERN_INFO"LCD driver exited.\n");

}

module_init(initFunction);
module_exit(exitFunction);