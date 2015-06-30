/* GMA30x motion sensor driver
 *
 *
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include <linux/interrupt.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/irq.h>
#include <linux/miscdevice.h>
#include <asm/uaccess.h>
#include <linux/delay.h>
#include <linux/input.h>
#include <linux/workqueue.h>
#include <linux/kobject.h>
#include <linux/earlysuspend.h>
#include <linux/platform_device.h>
#include <asm/atomic.h>

//#define POWER_NONE_MACRO MT65XX_POWER_NONE
//#define MT65XX_POWER_NONE	0
#include <cust_acc.h>
#include <linux/hwmsensor.h>
#include <linux/hwmsen_dev.h>
#include <linux/sensors_io.h>
#include "gma30x.h"
#include <linux/hwmsen_helper.h>
#include <mach/mt_typedefs.h>
#include <mach/mt_gpio.h>
#include <mach/mt_pm_ldo.h>


extern struct acc_hw* gma302_get_cust_acc_hw(void); 

/*----------------------------------------------------------------------------*/
#define I2C_DRIVERID_GMA30x 302
/*----------------------------------------------------------------------------*/

//#define _GMA302_SUPPORT_AUTO_DETECT_
#define ABS(a) ((a) < 0 ? -(a) : (a))
/*----------------------------------------------------------------------------*/
#define CONFIG_GMA30x_LOWPASS   /*apply low pass filter on output*/       
#define SW_CALIBRATION	// only use cali_sw, hw offset = 0
/*----------------------------------------------------------------------------*/
#define GMA30x_AXIS_X          0
#define GMA30x_AXIS_Y          1
#define GMA30x_AXIS_Z          2
#define GMA30x_AXES_NUM        3
#define GMA30x_DATA_LEN        11
#ifdef _GMA303_SUPPORT_AUTO_DETECT_
#define GMA30x_DEV_NAME        "gsensor"//"gma303"
#else
#define GMA30x_DEV_NAME        "gma302"
#endif
/*----------------------------------------------------------------------------*/
#define TRUE 1
#define FALSE 0
#if 0
static struct acc_hw cust_acc_hw = {
     .i2c_num = 4,
     .direction = 0,
     //.power_id = MT6516_POWER_NONE,  /*!< LDO is not used */
     //.power_vol= VOL_DEFAULT,        /*!< LDO is not used */
     .firlen   = 0,                  /*!< don't enable low pass filter */
  };
#endif

static const struct i2c_device_id gma302_i2c_id[] = {{GMA30x_DEV_NAME,0},{}};
static struct i2c_board_info __initdata i2c_gma302={ I2C_BOARD_INFO("gma302", 0x18)};
/*the adapter id will be available in customization*/
//static unsigned short gma302_force[] = {0x00, GMA1302_I2C_SLAVE_ADDR, I2C_CLIENT_END, I2C_CLIENT_END};
//static const unsigned short *const gma302_forces[] = { gma302_force, NULL };
//static struct i2c_client_address_data gma302_addr_data = { .forces = gma302_forces,};

/*----------------------------------------------------------------------------*/
static int gma30x_i2c_probe(struct i2c_client *client, const struct i2c_device_id *id); 
static int gma30x_i2c_remove(struct i2c_client *client);
static int gma30x_i2c_detect(struct i2c_client *client, int kind, struct i2c_board_info *info);
static int gma30x_suspend(struct i2c_client *client, pm_message_t msg) ;
static int gma30x_resume(struct i2c_client *client);

#ifdef _GMA302_SUPPORT_AUTO_DETECT_
    static int gma30x_local_init(void);
    static int gma30x_remove(void);
#endif

/*----------------------------------------------------------------------------*/
typedef enum {
    ADX_TRC_FILTER  =     0x01,
    ADX_TRC_RAWDATA =     0x02,
    ADX_TRC_IOCTL   =     0x04,
    ADX_TRC_CALI	= 0X08,
    ADX_TRC_INFO	= 0X10,
} ADX_TRC;
/*----------------------------------------------------------------------------*/
struct scale_factor{
    u8  whole;
    u8  fraction;
};
/*----------------------------------------------------------------------------*/
struct data_resolution {
    struct scale_factor scalefactor;
    int                 sensitivity;
};
/*----------------------------------------------------------------------------*/
#define C_MAX_FIR_LENGTH (32)
/*----------------------------------------------------------------------------*/
struct data_filter {
    s16 raw[C_MAX_FIR_LENGTH][GMA30x_AXES_NUM];
    int sum[GMA30x_AXES_NUM];
    int num;
    int idx;
};
/*----------------------------------------------------------------------------*/
struct gma30x_i2c_data {
    struct i2c_client *client;
    struct acc_hw *hw;
    struct hwmsen_convert   cvt;
    
    /*misc*/
    struct data_resolution *reso;
    atomic_t                trace;
    atomic_t                suspend;
    atomic_t                selftest;
	atomic_t				filter;
    s16                     cali_sw[GMA30x_AXES_NUM+1];

    /*data*/
    s8                      offset[GMA30x_AXES_NUM+1];  /*+1: for 4-byte alignment*/
    s16                     data[GMA30x_AXES_NUM+1];
#if defined(CONFIG_GMA30x_LOWPASS)
    atomic_t                firlen;
    atomic_t                fir_en;
    struct data_filter      fir;
#endif 
    /*early suspend*/
#if defined(CONFIG_HAS_EARLYSUSPEND)
    struct early_suspend    early_drv;
#endif     
};
/*----------------------------------------------------------------------------*/
static struct i2c_driver gma30x_i2c_driver = {
    .driver = {
//        .owner          = THIS_MODULE,
        .name           = GMA30x_DEV_NAME,
    },
	.probe      		= gma30x_i2c_probe,
	.remove    			= gma30x_i2c_remove,
	.detect				= gma30x_i2c_detect,
#if !defined(CONFIG_HAS_EARLYSUSPEND)    
    .suspend            = gma30x_suspend,
    .resume             = gma30x_resume,
#endif
	.id_table = gma302_i2c_id,
	//.address_list= gma302_forces,
};

/*----------------------------------------------------------------------------*/
static struct i2c_client *gma30x_i2c_client = NULL;
#ifdef _GMA302_SUPPORT_AUTO_DETECT_
    static struct sensor_init_info  gma302_init_info =
    {
        .name   = GMA30x_DEV_NAME,
        .init   = gma30x_local_init,
        .uninit = gma30x_remove,
    };
#else
	static struct platform_driver gma30x_gsensor_driver;
#endif
static DEFINE_MUTEX(i2c_mutex);
static struct gma30x_i2c_data *obj_i2c_data = NULL;
static bool sensor_power = true;//false;
static bool enable_status = false;

static GSENSOR_VECTOR3D gsensor_gain;
static char selftestRes[8]= {0}; 

/*----------------------------------------------------------------------------*/
//#define DEBUG	/**< if define : Enable gma->client->dev debug data .*/
#define GSE_TAG                  "[Gsensor] "
#ifdef DEBUG
#define GSE_FUN(f)               printk(KERN_INFO GSE_TAG"%s %d\n", __FUNCTION__, __LINE__)
#define GSE_ERR(fmt, args...)    printk(KERN_ERR GSE_TAG"%s %d : "fmt, __FUNCTION__, __LINE__, ##args)
#define GSE_LOG(fmt, args...)    printk(KERN_INFO GSE_TAG fmt, ##args)
#else
#define GSE_ERR(fmt, args...)
#define GSE_LOG(fmt, args...)
#define GSE_FUN(f)
#endif
/*----------------------------------------------------------------------------*/
static struct data_resolution gma30x_data_resolution[] = {
 /*8 combination by {FULL_RES,RANGE}*/
	 {{ 3, 9}, 512},   /*+/-16g in 13-bit resolution:  3.9 mg/LSB (full-resolution)*/           
};
/*----------------------------------------------------------------------------*/
static struct data_resolution gma30x_offset_resolution = {{15, 6}, 64};
/*--------------------ADXL power control function----------------------------------*/
static void GMA302_power(struct acc_hw *hw, unsigned int on) 
{
	static unsigned int power_on = 0;
/*
	if(hw->power_id != POWER_NONE_MACRO)		// have externel LDO
	{        
		GSE_LOG("power %s\n", on ? "on" : "off");
		if(power_on == on)	// power status not change
		{
			GSE_LOG("ignore power control: %d\n", on);
		}
		else if(on)	// power on
		{
			if(!hwPowerOn(hw->power_id, hw->power_vol, "GMA302"))
			{
				GSE_ERR("power on fails!!\n");
			}
		}
		else	// power off
		{
			if (!hwPowerDown(hw->power_id, "GMA302"))
			{
				GSE_ERR("power off fail!!\n");
			}			  
		}
	}*/
	power_on = on;    
}
/*----------------------------------------------------------------------------*/
static int GMA30x_SetDataResolution(struct gma30x_i2c_data *obj)
{
	//int err;
	//u8  dat, reso;
	obj->reso = &gma30x_data_resolution[0];
	return 0;
}
/*----------------------------------------------------------------------------*/
/* I2C I/O function */
static int GMA30x_ReadData(struct i2c_client *client, s16 data[GMA30x_AXES_NUM])
{
	struct gma30x_i2c_data *priv = i2c_get_clientdata(client);        
	u8 addr = GMA1302_REG_STADR;
	u8 buffer[GMA30x_DATA_LEN] = {0};
	int i, err = 0;
	buffer[0] = GMA1302_REG_STADR;
	
	mutex_lock(&i2c_mutex);
	
	if(NULL == client)
	{
		err = -EINVAL;
	}
	else if((err = hwmsen_read_block(client, addr, buffer, 0x03)))
	{
		dev_err(&priv->client->dev, "Read acceleration data fail\n");
	}
	else  if((err = hwmsen_read_block(client, 0x06, buffer, 0x06)))
	{
		dev_err(&priv->client->dev, "Read acceleration data fail\n");
	}
	else
	{
		/* merge xyz high/low bytes(13bit) & 1g = 512 */
		for(i = 0; i < SENSOR_DATA_SIZE; ++i)
		{	 
			data[i] = (s16)((buffer[2 * i + 1] << 8) | buffer[2 * i] );
		//	GSE_LOG("data[%d]=%d, %d\n", i, data[i], __LINE__);
		}		

		if(atomic_read(&priv->trace) & ADX_TRC_RAWDATA)
		{
			GSE_LOG("[%08X %08X %08X] => [%5d %5d %5d]\n", data[GMA30x_AXIS_X], data[GMA30x_AXIS_Y], data[GMA30x_AXIS_Z],
		                               data[GMA30x_AXIS_X], data[GMA30x_AXIS_Y], data[GMA30x_AXIS_Z]);
		}
#ifdef CONFIG_GMA30x_LOWPASS
		if(atomic_read(&priv->filter))
		{
			if(atomic_read(&priv->fir_en) && !atomic_read(&priv->suspend))
			{
				int idx, firlen = atomic_read(&priv->firlen);   
				if(priv->fir.num < firlen)
				{                
					priv->fir.raw[priv->fir.num][GMA30x_AXIS_X] = data[GMA30x_AXIS_X];
					priv->fir.raw[priv->fir.num][GMA30x_AXIS_Y] = data[GMA30x_AXIS_Y];
					priv->fir.raw[priv->fir.num][GMA30x_AXIS_Z] = data[GMA30x_AXIS_Z];
					priv->fir.sum[GMA30x_AXIS_X] += data[GMA30x_AXIS_X];
					priv->fir.sum[GMA30x_AXIS_Y] += data[GMA30x_AXIS_Y];
					priv->fir.sum[GMA30x_AXIS_Z] += data[GMA30x_AXIS_Z];
					if(atomic_read(&priv->trace) & ADX_TRC_FILTER)
					{
						GSE_LOG("add [%2d] [%5d %5d %5d] => [%5d %5d %5d]\n", priv->fir.num,
							priv->fir.raw[priv->fir.num][GMA30x_AXIS_X], priv->fir.raw[priv->fir.num][GMA30x_AXIS_Y], priv->fir.raw[priv->fir.num][GMA30x_AXIS_Z],
							priv->fir.sum[GMA30x_AXIS_X], priv->fir.sum[GMA30x_AXIS_Y], priv->fir.sum[GMA30x_AXIS_Z]);
					}
					priv->fir.num++;
					priv->fir.idx++;
				}
				else
				{
					idx = priv->fir.idx % firlen;
					priv->fir.sum[GMA30x_AXIS_X] -= priv->fir.raw[idx][GMA30x_AXIS_X];
					priv->fir.sum[GMA30x_AXIS_Y] -= priv->fir.raw[idx][GMA30x_AXIS_Y];
					priv->fir.sum[GMA30x_AXIS_Z] -= priv->fir.raw[idx][GMA30x_AXIS_Z];
					priv->fir.raw[idx][GMA30x_AXIS_X] = data[GMA30x_AXIS_X];
					priv->fir.raw[idx][GMA30x_AXIS_Y] = data[GMA30x_AXIS_Y];
					priv->fir.raw[idx][GMA30x_AXIS_Z] = data[GMA30x_AXIS_Z];
					priv->fir.sum[GMA30x_AXIS_X] += data[GMA30x_AXIS_X];
					priv->fir.sum[GMA30x_AXIS_Y] += data[GMA30x_AXIS_Y];
					priv->fir.sum[GMA30x_AXIS_Z] += data[GMA30x_AXIS_Z];
					priv->fir.idx++;
					data[GMA30x_AXIS_X] = priv->fir.sum[GMA30x_AXIS_X]/firlen;
					data[GMA30x_AXIS_Y] = priv->fir.sum[GMA30x_AXIS_Y]/firlen;
					data[GMA30x_AXIS_Z] = priv->fir.sum[GMA30x_AXIS_Z]/firlen;
					if(atomic_read(&priv->trace) & ADX_TRC_FILTER)
					{
						GSE_LOG("add [%2d] [%5d %5d %5d] => [%5d %5d %5d] : [%5d %5d %5d]\n", idx,
						priv->fir.raw[idx][GMA30x_AXIS_X], priv->fir.raw[idx][GMA30x_AXIS_Y], priv->fir.raw[idx][GMA30x_AXIS_Z],
						priv->fir.sum[GMA30x_AXIS_X], priv->fir.sum[GMA30x_AXIS_Y], priv->fir.sum[GMA30x_AXIS_Z],
						data[GMA30x_AXIS_X], data[GMA30x_AXIS_Y], data[GMA30x_AXIS_Z]);
					}
				}
			}
		}	
#endif         
	}
	mutex_unlock(&i2c_mutex);

	return err;
}
/*----------------------------------------------------------------------------*/
static int GMA30x_ReadOffset(struct i2c_client *client, s8 ofs[GMA30x_AXES_NUM])
{    
	int err=0;
#ifdef SW_CALIBRATION
	ofs[0]=ofs[1]=ofs[2]=0x0;
#else
	
#endif
	dev_dbg(&client->dev, "offesx=%x, y=%x, z=%x",ofs[0],ofs[1],ofs[2]);
	
	return err;
}
/*----------------------------------------------------------------------------*/
static int GMA30x_ResetCalibration(struct i2c_client *client)
{
	struct gma30x_i2c_data *obj = i2c_get_clientdata(client);
	//s8 ofs[GMA30x_AXES_NUM] = {0x00, 0x00, 0x00};
	int err =0;
	
	#ifdef SW_CALIBRATION
		
	#else
		
	#endif

	memset(obj->cali_sw, 0x00, sizeof(obj->cali_sw));
	memset(obj->offset, 0x00, sizeof(obj->offset));
	return err;    
}
/*----------------------------------------------------------------------------*/
static int GMA30x_ReadCalibration(struct i2c_client *client, int dat[GMA30x_AXES_NUM])
{
    struct gma30x_i2c_data *obj = i2c_get_clientdata(client);
    int err=0;
    int mul;

	#ifdef SW_CALIBRATION
		mul = 0;//only SW Calibration, disable HW Calibration
	#else
	    
	#endif

    dat[obj->cvt.map[GMA30x_AXIS_X]] = obj->cvt.sign[GMA30x_AXIS_X]*(obj->offset[GMA30x_AXIS_X]*mul + obj->cali_sw[GMA30x_AXIS_X]);
    dat[obj->cvt.map[GMA30x_AXIS_Y]] = obj->cvt.sign[GMA30x_AXIS_Y]*(obj->offset[GMA30x_AXIS_Y]*mul + obj->cali_sw[GMA30x_AXIS_Y]);
    dat[obj->cvt.map[GMA30x_AXIS_Z]] = obj->cvt.sign[GMA30x_AXIS_Z]*(obj->offset[GMA30x_AXIS_Z]*mul + obj->cali_sw[GMA30x_AXIS_Z]);                        
                                       
    return err;
}
/*----------------------------------------------------------------------------*/
static int GMA30x_ReadCalibrationEx(struct i2c_client *client, int act[GMA30x_AXES_NUM], int raw[GMA30x_AXES_NUM])
{  
	/*raw: the raw calibration data; act: the actual calibration data*/
	struct gma30x_i2c_data *obj = i2c_get_clientdata(client);
	int err=0;
	int mul;

	#ifdef SW_CALIBRATION
		mul = 0;//only SW Calibration, disable HW Calibration
	#else
		
	#endif

	//mul = obj->reso->sensitivity/gma30x_offset_resolution.sensitivity;
	raw[GMA30x_AXIS_X] = obj->offset[GMA30x_AXIS_X]*mul + obj->cali_sw[GMA30x_AXIS_X];
	raw[GMA30x_AXIS_Y] = obj->offset[GMA30x_AXIS_Y]*mul + obj->cali_sw[GMA30x_AXIS_Y];
	raw[GMA30x_AXIS_Z] = obj->offset[GMA30x_AXIS_Z]*mul + obj->cali_sw[GMA30x_AXIS_Z];

	act[obj->cvt.map[GMA30x_AXIS_X]] = obj->cvt.sign[GMA30x_AXIS_X]*raw[GMA30x_AXIS_X];
	act[obj->cvt.map[GMA30x_AXIS_Y]] = obj->cvt.sign[GMA30x_AXIS_Y]*raw[GMA30x_AXIS_Y];
	act[obj->cvt.map[GMA30x_AXIS_Z]] = obj->cvt.sign[GMA30x_AXIS_Z]*raw[GMA30x_AXIS_Z];                        
	                       
	return err;
}
/*----------------------------------------------------------------------------*/
static int GMA30x_WriteCalibration(struct i2c_client *client, int dat[GMA30x_AXES_NUM])
{
	struct gma30x_i2c_data *obj = i2c_get_clientdata(client);
	int err;
	int cali[GMA30x_AXES_NUM], raw[GMA30x_AXES_NUM];
	//int lsb = gma30x_offset_resolution.sensitivity;
	//int divisor = obj->reso->sensitivity/lsb;

	if((err = GMA30x_ReadCalibrationEx(client, cali, raw)))	/*offset will be updated in obj->offset*/
	{ 
		GSE_ERR("read offset fail, %d\n", err);
		return err;
	}

	GSE_LOG("OLDOFF: (%+3d %+3d %+3d): (%+3d %+3d %+3d) / (%+3d %+3d %+3d)\n", 
		raw[GMA30x_AXIS_X], raw[GMA30x_AXIS_Y], raw[GMA30x_AXIS_Z],
		obj->offset[GMA30x_AXIS_X], obj->offset[GMA30x_AXIS_Y], obj->offset[GMA30x_AXIS_Z],
		obj->cali_sw[GMA30x_AXIS_X], obj->cali_sw[GMA30x_AXIS_Y], obj->cali_sw[GMA30x_AXIS_Z]);

	/*calculate the real offset expected by caller*/
	cali[GMA30x_AXIS_X] += dat[GMA30x_AXIS_X];
	cali[GMA30x_AXIS_Y] += dat[GMA30x_AXIS_Y];
	cali[GMA30x_AXIS_Z] += dat[GMA30x_AXIS_Z];

	GSE_LOG("UPDATE: (%+3d %+3d %+3d)\n", 
		dat[GMA30x_AXIS_X], dat[GMA30x_AXIS_Y], dat[GMA30x_AXIS_Z]);

#ifdef SW_CALIBRATION
	obj->cali_sw[GMA30x_AXIS_X] = obj->cvt.sign[GMA30x_AXIS_X]*(cali[obj->cvt.map[GMA30x_AXIS_X]]);
	obj->cali_sw[GMA30x_AXIS_Y] = obj->cvt.sign[GMA30x_AXIS_Y]*(cali[obj->cvt.map[GMA30x_AXIS_Y]]);
	obj->cali_sw[GMA30x_AXIS_Z] = obj->cvt.sign[GMA30x_AXIS_Z]*(cali[obj->cvt.map[GMA30x_AXIS_Z]]);	
	GSE_LOG("UPDATE: (%+3d %+3d %+3d)==%d\n", 
		obj->cali_sw[GMA30x_AXIS_X], obj->cali_sw[GMA30x_AXIS_Y], obj->cali_sw[GMA30x_AXIS_Z], __LINE__);
#else
	obj->offset[GMA30x_AXIS_X] = (s8)(obj->cvt.sign[GMA30x_AXIS_X]*(cali[obj->cvt.map[GMA30x_AXIS_X]])/(divisor));
	obj->offset[GMA30x_AXIS_Y] = (s8)(obj->cvt.sign[GMA30x_AXIS_Y]*(cali[obj->cvt.map[GMA30x_AXIS_Y]])/(divisor));
	obj->offset[GMA30x_AXIS_Z] = (s8)(obj->cvt.sign[GMA30x_AXIS_Z]*(cali[obj->cvt.map[GMA30x_AXIS_Z]])/(divisor));

	/*convert software calibration using standard calibration*/
	obj->cali_sw[GMA30x_AXIS_X] = obj->cvt.sign[GMA30x_AXIS_X]*(cali[obj->cvt.map[GMA30x_AXIS_X]])%(divisor);
	obj->cali_sw[GMA30x_AXIS_Y] = obj->cvt.sign[GMA30x_AXIS_Y]*(cali[obj->cvt.map[GMA30x_AXIS_Y]])%(divisor);
	obj->cali_sw[GMA30x_AXIS_Z] = obj->cvt.sign[GMA30x_AXIS_Z]*(cali[obj->cvt.map[GMA30x_AXIS_Z]])%(divisor);

	GSE_LOG("NEWOFF: (%+3d %+3d %+3d): (%+3d %+3d %+3d) / (%+3d %+3d %+3d)\n", 
		obj->offset[GMA30x_AXIS_X]*divisor + obj->cali_sw[GMA30x_AXIS_X], 
		obj->offset[GMA30x_AXIS_Y]*divisor + obj->cali_sw[GMA30x_AXIS_Y], 
		obj->offset[GMA30x_AXIS_Z]*divisor + obj->cali_sw[GMA30x_AXIS_Z], 
		obj->offset[GMA30x_AXIS_X], obj->offset[GMA30x_AXIS_Y], obj->offset[GMA30x_AXIS_Z],
		obj->cali_sw[GMA30x_AXIS_X], obj->cali_sw[GMA30x_AXIS_Y], obj->cali_sw[GMA30x_AXIS_Z]);

	
#endif

	return err;
}
/*----------------------------------------------------------------------------*/
int gma30x_ok = 0;
static int GMA30x_CheckDeviceID(struct i2c_client *client)
{
	//GSE_FUN();
	u8 databuf[5];   	int res = 0;

	memset(databuf, 0, sizeof(u8)*2);

	mutex_lock(&i2c_mutex);
	
	/* 1. Powerdown reset */
	databuf[0] = GMA1302_REG_PD;
	databuf[1] = GMA1302_MODE_RESET;
	res = i2c_master_send(client, databuf, 0x2);
	if(res < 0)
		goto exit_GMA30x_CheckDeviceID;
	
	/* 2. check GMA1302_REG_STADR(0x04) */
	databuf[0] = GMA1302_REG_STADR;    

	res = i2c_master_send(client, databuf, 0x1);
	if(res <= 0)
		goto exit_GMA30x_CheckDeviceID;
	
	udelay(500);

	databuf[0] = GMA1302_REG_STADR;        
	res = i2c_master_recv(client, databuf, 0x01);
	if(res <= 0)
		goto exit_GMA30x_CheckDeviceID;
	
	if(databuf[0] == GMA30x_VAL_WMI)
		GSE_LOG("%s: PID = 0x%x, GMA30x accelerometer\n", __func__, databuf[0]);
	else{
		mutex_unlock(&i2c_mutex);
		GSE_ERR("%s: PID = 0x%x, The device is not GlobalMems accelerometer.", __func__, databuf[0]);
		return GMA30x_ERR_IDENTIFICATION;
	}
	/* 3. turn off the high-pass filter ,turn on the low-pass filter*/
	databuf[0] = GMA1302_REG_CONTR1;
	databuf[1] = GMA1302_VAL_OFF | GMA1302_VAL_LPF_ON;

	res = i2c_master_send(client, databuf, 0x2);
	if(res < 0)
		goto exit_GMA30x_CheckDeviceID;
	/* 4. turn on the offset temperature compensation */
	databuf[0] = GMA1302_REG_CONTR3;
	databuf[1] = GMA1302_VAL_OFF;//GMA1302_VAL_OFFSET_TC_ON;
	res = i2c_master_send(client, databuf, 0x2);
	if(res < 0)
		goto exit_GMA30x_CheckDeviceID;
	/* 5. turn off the data ready interrupt and configure the INT pin to active high, push-pull type */
	databuf[0] = GMA1302_REG_INTCR;
	databuf[1] = GMA1302_VAL_OFF;
	res = i2c_master_send(client, databuf, 0x2);
	if(res < 0)
		goto exit_GMA30x_CheckDeviceID;
	/* 6. treshold set to max */
	databuf[0] = GMA1302_REG_MTHR;
	databuf[1] = GMA1302_VAL_TRESHOLD_MAX;
	res = i2c_master_send(client, databuf, 0x2);
	if(res < 0)
		goto exit_GMA30x_CheckDeviceID;
	/* 7. Oversampling mode & Set Action register */
/*	databuf[0] = GMA1302_REG_OSM;
	databuf[1] = GMA1302_VAL_LOW_NOISE;//Low noise
	res = i2c_master_send(client, databuf, 0x2);
	if(res < 0)
		goto exit_GMA30x_CheckDeviceID;
	databuf[0] = GMA1302_REG_ACTR;
	databuf[1] = GMA1302_VAL_ACTR_CONTINUOUS;
	databuf[2] = GMA1302_VAL_ACTR_RESET;
	databuf[3] = GMA1302_VAL_ACTR_NON_CONTINUOUS;
	databuf[4] = GMA1302_VAL_ACTR_RESET;
	res = i2c_master_send(client, databuf, 0x5);
	if(res < 0)
		goto exit_GMA30x_CheckDeviceID;
*/	
exit_GMA30x_CheckDeviceID:
	mutex_unlock(&i2c_mutex);
	if (res <= 0)
		return GMA30x_ERR_I2C;
	//zhj add 
	gma30x_ok = 1;
	//zhj end
	return GMA30x_SUCCESS;
}
/*----------------------------------------------------------------------------*/
static int GMA30x_SetPowerMode(struct i2c_client *client, bool enable)
{
	u8 databuf[2];    
	int res = 0;
	//u8 addr = GMA1302_REG_PD;
	//struct gma30x_i2c_data *obj = i2c_get_clientdata(client);
	
	
	if(enable == sensor_power)
	{
		GSE_LOG("Sensor power status is newest!\n");
		return GMA30x_SUCCESS;
	}
/*
	if(hwmsen_read_block(client, addr, databuf, 0x01))
	{
		GSE_ERR("read power ctl register err!\n");
		return GMA30x_ERR_I2C;
	}
*/	
	if(enable == TRUE)
	{
		res = GMA30x_CheckDeviceID(client); 
		if(res != GMA30x_SUCCESS){
			GSE_ERR("Check ID error\n");
			return res;
		}
	}
	else
	{
		databuf[0] = GMA1302_REG_PD;
		databuf[1] = GMA1302_MODE_POWERDOWN;
		mutex_lock(&i2c_mutex);
		res = i2c_master_send(client, databuf, 0x2);
		mutex_unlock(&i2c_mutex);
	}

	sensor_power = enable;
	
	return GMA30x_SUCCESS;    
}
/*
static int GMA302_SetBWRate(struct i2c_client *client, u8 bwrate)
{
	u8 databuf[10];    
	int res = 0;

	memset(databuf, 0, sizeof(u8)*10);    

	return GMA30x_SUCCESS;    
}
static int GMA302_SetIntEnable(struct i2c_client *client, u8 intenable)
{
	//u8 databuf[10];    
	//int res = 0;
	//for disable interrupt function/
	
	return GMA30x_SUCCESS;   
}
*/
static int gma30x_init_client(struct i2c_client *client, int reset_cali)
{
	struct gma30x_i2c_data *obj = i2c_get_clientdata(client);
	int res = 0;
	
	res = GMA30x_CheckDeviceID(client); 
	if(res != GMA30x_SUCCESS){
	    GSE_ERR("Check ID error\n");
		return res;
	}	

	//res = GMA30x_SetPowerMode(client, false);
	if(res != GMA30x_SUCCESS)
	{
	    GSE_ERR("set power error\n");
		return res;
	}
	
	res = GMA30x_SetDataResolution(obj);
	if(res != GMA30x_SUCCESS)
	{
	    GSE_ERR("set data format error\n");
		return res;
	}

	gsensor_gain.x = gsensor_gain.y = gsensor_gain.z = obj->reso->sensitivity;

	if(0 != reset_cali)
	{ 
		/*reset calibration only in power on*/
		res = GMA30x_ResetCalibration(client);
		if(res != GMA30x_SUCCESS)
		{
			return res;
		}
	}
	GSE_LOG("gma30x_init_client OK!\n");
#ifdef CONFIG_GMA30x_LOWPASS
	memset(&obj->fir, 0x00, sizeof(obj->fir));  
#endif

	return GMA30x_SUCCESS;
}
/*----------------------------------------------------------------------------*/
static int GMA30x_ReadChipInfo(struct i2c_client *client, char *buf, int bufsize)
{
	u8 databuf[10];    

	memset(databuf, 0, sizeof(u8)*10);

	if((NULL == buf)||(bufsize<=30))
	{
		return -1;
	}
	
	if(NULL == client)
	{
		*buf = 0;
		return -2;
	}

	sprintf(buf, "GMA302 Chip");
	return 0;
}
/*----------------------------------------------------------------------------*/
static int GMA30x_ReadSensorData(struct i2c_client *client, char *buf, int bufsize)
{
	int acc[GMA30x_AXES_NUM];
	int i, res = 0;	
	struct gma30x_i2c_data *obj = obj_i2c_data; //(struct gma30x_i2c_data*)i2c_get_clientdata(client);
	client = obj->client;
	//u8 databuf[20];
	
	//memset(databuf, 0, sizeof(u8)*10);

	if(NULL == buf)
	{
		return -1;
	}
	if(NULL == client)
	{
		*buf = 0;
		return -2;
	}

	if(sensor_power == FALSE)
	{
		res = GMA30x_SetPowerMode(client, true);
		if(res)
		{
			dev_err(&client->dev, "Power on gma30x error %d!\n", res);
		}
		msleep(20);
	}

	if((res = GMA30x_ReadData(client, obj->data)))
	{        
		GSE_ERR("I2C error: ret value=%d\n", res);
		return -3;
	}
	else
	{
		obj->data[GMA30x_AXIS_X] += obj->cali_sw[GMA30x_AXIS_X];
		obj->data[GMA30x_AXIS_Y] += obj->cali_sw[GMA30x_AXIS_Y];
		obj->data[GMA30x_AXIS_Z] += obj->cali_sw[GMA30x_AXIS_Z];
		for(i = 0; i < SENSOR_DATA_SIZE; ++i)	
			dev_dbg(&client->dev, "obj->data[%d]=%d, obj->cali_sw[%d]=%d :%d\n", i, obj->data[i], i, obj->cali_sw[i], __LINE__);
		/*remap coordinate*/
		acc[obj->cvt.map[GMA30x_AXIS_X]] = obj->cvt.sign[GMA30x_AXIS_X]*obj->data[GMA30x_AXIS_X];
		acc[obj->cvt.map[GMA30x_AXIS_Y]] = obj->cvt.sign[GMA30x_AXIS_Y]*obj->data[GMA30x_AXIS_Y];
		acc[obj->cvt.map[GMA30x_AXIS_Z]] = obj->cvt.sign[GMA30x_AXIS_Z]*obj->data[GMA30x_AXIS_Z];
		//for(i = 0; i < SENSOR_DATA_SIZE; ++i)
			//dev_dbg(&client->dev, "acc[obj->cvt.map[%d]]=%d, obj->cvt.sign[%d]=%d * obj->data[%d]=%d:%d\n", i, acc[i], i, obj->cvt.sign[i], i, obj->data[i], __LINE__);
		/* Analyzing horizontal state 2015-05-16 add */
		if( ABS(acc[GMA30x_AXIS_X]) < 21 && ABS(acc[GMA30x_AXIS_Y]) < 21)
		{
			acc[GMA30x_AXIS_X] = 0;
			acc[GMA30x_AXIS_Y] = 0;
		}
		//Out put the mg
		acc[GMA30x_AXIS_X] = acc[GMA30x_AXIS_X] * GRAVITY_EARTH_1000 / obj->reso->sensitivity;
		acc[GMA30x_AXIS_Y] = acc[GMA30x_AXIS_Y] * GRAVITY_EARTH_1000 / obj->reso->sensitivity;
		acc[GMA30x_AXIS_Z] = acc[GMA30x_AXIS_Z] * GRAVITY_EARTH_1000 / obj->reso->sensitivity;		
		for(i = 0; i < SENSOR_DATA_SIZE; ++i)
			dev_dbg(&client->dev, "acc[%d]=%d, GRAVITY_EARTH_1000 =%d , obj->reso->sensitivity =%d :%d\n", i, acc[i], GRAVITY_EARTH_1000, obj->reso->sensitivity, __LINE__);

		sprintf(buf, "%04x %04x %04x", acc[GMA30x_AXIS_X], acc[GMA30x_AXIS_Y], acc[GMA30x_AXIS_Z]);
		dev_dbg(&client->dev, "%04x %04x %04x :%d\n", acc[GMA30x_AXIS_X], acc[GMA30x_AXIS_Y], acc[GMA30x_AXIS_Z],__LINE__);
		if(atomic_read(&obj->trace) & ADX_TRC_IOCTL)
		{
			dev_dbg(&client->dev, "gsensor data: %s!\n", buf);
		}
	}
	
	return 0;
}
/*----------------------------------------------------------------------------*/
static int GMA30x_ReadRawData(struct i2c_client *client, char *buf)
{
	struct gma30x_i2c_data *obj = (struct gma30x_i2c_data*)i2c_get_clientdata(client);
	int res = 0;

	if (!buf || !client)
		return EINVAL;
	
	if((res = GMA30x_ReadData(client, obj->data))){        
		GSE_ERR("I2C error: ret value=%d", res);
		return EIO;
	}
	else{
		sprintf(buf, "%04x %04x %04x", obj->data[GMA30x_AXIS_X], 
			obj->data[GMA30x_AXIS_Y], obj->data[GMA30x_AXIS_Z]);
	}
	
	return 0;
}
/*----------------------------------------------------------------------------*/
static int GMA30x_InitSelfTest(struct i2c_client *client)
{
	//int res = 0;
	//u8  data;
	
	return GMA30x_SUCCESS;
}
/*----------------------------------------------------------------------------*/
static ssize_t show_chipinfo_value(struct device_driver *ddri, char *buf)
{
	struct i2c_client *client = gma30x_i2c_client;
	char strbuf[GMA30x_BUFSIZE];
	if(NULL == client)
	{
		GSE_ERR("i2c client is null!!\n");
		return 0;
	}
	
	GMA30x_ReadChipInfo(client, strbuf, GMA30x_BUFSIZE);
	return snprintf(buf, PAGE_SIZE, "%s\n", strbuf);        
}
/*----------------------------------------------------------------------------*/
static ssize_t show_sensordata_value(struct device_driver *ddri, char *buf)
{
	struct i2c_client *client = gma30x_i2c_client;
	char strbuf[GMA30x_BUFSIZE];
	
	if(NULL == client)
	{
		GSE_ERR("i2c client is null!!\n");
		return 0;
	}
	GMA30x_ReadSensorData(client, strbuf, GMA30x_BUFSIZE);
	return snprintf(buf, PAGE_SIZE, "%s\n", strbuf);            
}
/*----------------------------------------------------------------------------*/
static ssize_t show_cali_value(struct device_driver *ddri, char *buf)
{
	struct i2c_client *client = gma30x_i2c_client;
	struct gma30x_i2c_data *obj;
	int err, len = 0, mul;
	int tmp[GMA30x_AXES_NUM];

	if(NULL == client)
	{
		GSE_ERR("i2c client is null!!\n");
		return 0;
	}

	obj = i2c_get_clientdata(client);



	if((err = GMA30x_ReadOffset(client, obj->offset)))
	{
		return -EINVAL;
	}
	else if((err = GMA30x_ReadCalibration(client, tmp)))
	{
		return -EINVAL;
	}
	else
	{
		mul = obj->reso->sensitivity/gma30x_offset_resolution.sensitivity;
		len += snprintf(buf+len, PAGE_SIZE-len, "[HW ][%d] (%+3d, %+3d, %+3d) : (0x%02X, 0x%02X, 0x%02X)\n", mul,                        
			obj->offset[GMA30x_AXIS_X], obj->offset[GMA30x_AXIS_Y], obj->offset[GMA30x_AXIS_Z],
			obj->offset[GMA30x_AXIS_X], obj->offset[GMA30x_AXIS_Y], obj->offset[GMA30x_AXIS_Z]);
		len += snprintf(buf+len, PAGE_SIZE-len, "[SW ][%d] (%+3d, %+3d, %+3d)\n", 1, 
			obj->cali_sw[GMA30x_AXIS_X], obj->cali_sw[GMA30x_AXIS_Y], obj->cali_sw[GMA30x_AXIS_Z]);

		len += snprintf(buf+len, PAGE_SIZE-len, "[ALL]    (%+3d, %+3d, %+3d) : (%+3d, %+3d, %+3d)\n", 
			obj->offset[GMA30x_AXIS_X]*mul + obj->cali_sw[GMA30x_AXIS_X],
			obj->offset[GMA30x_AXIS_Y]*mul + obj->cali_sw[GMA30x_AXIS_Y],
			obj->offset[GMA30x_AXIS_Z]*mul + obj->cali_sw[GMA30x_AXIS_Z],
			tmp[GMA30x_AXIS_X], tmp[GMA30x_AXIS_Y], tmp[GMA30x_AXIS_Z]);
		
		return len;
    }
}
/*----------------------------------------------------------------------------*/
static ssize_t store_cali_value(struct device_driver *ddri, const char *buf, size_t count)
{
	struct i2c_client *client = gma30x_i2c_client;  
	int err, x, y, z;
	int dat[GMA30x_AXES_NUM];
	int i;
	if(!strncmp(buf, "rst", 3))
	{
		if((err = GMA30x_ResetCalibration(client)))
		{
			GSE_ERR("reset offset err = %d\n", err);
		}	
	}
	else if(3 == sscanf(buf, "0x%02X 0x%02X 0x%02X", &x, &y, &z))
	{
		dat[GMA30x_AXIS_X] = x;
		dat[GMA30x_AXIS_Y] = y;
		dat[GMA30x_AXIS_Z] = z;
		for(i = 0; i < SENSOR_DATA_SIZE; ++i)	
			dev_dbg(&client->dev, "dat[%d]=%d :%d\n", i, dat[i], __LINE__);
		if((err = GMA30x_WriteCalibration(client, dat)))
		{
			GSE_ERR("write calibration err = %d\n", err);
		}		
	}
	else
	{
		GSE_ERR("invalid format\n");
	}
	
	return count;
}
/*----------------------------------------------------------------------------*/
static ssize_t show_self_value(struct device_driver *ddri, char *buf)
{
	struct i2c_client *client = gma30x_i2c_client;
	//struct gma30x_i2c_data *obj;

	if(NULL == client)
	{
		GSE_ERR("i2c client is null!!\n");
		return 0;
	}

	//obj = i2c_get_clientdata(client);

    return snprintf(buf, 8, "%s\n", selftestRes);
}
/*----------------------------------------------------------------------------*/
static ssize_t store_self_value(struct device_driver *ddri, const char *buf, size_t count)
{   /*write anything to this register will trigger the process*/
	struct item{
		s16 raw[GMA30x_AXES_NUM];
	};
	
	struct i2c_client *client = gma30x_i2c_client;  
	int idx, res, num;
	struct item *prv = NULL, *nxt = NULL;
	s32 avg_prv[GMA30x_AXES_NUM] = {0, 0, 0};
	s32 avg_nxt[GMA30x_AXES_NUM] = {0, 0, 0};


	if(1 != sscanf(buf, "%d", &num))
	{
		GSE_ERR("parse number fail\n");
		return count;
	}
	else if(num == 0)
	{
		GSE_ERR("invalid data count\n");
		return count;
	}

	prv = kzalloc(sizeof(*prv) * num, GFP_KERNEL);
	nxt = kzalloc(sizeof(*nxt) * num, GFP_KERNEL);
	if (!prv || !nxt)
	{
		goto exit;
	}


	GSE_LOG("NORMAL:\n");
	GMA30x_SetPowerMode(client,true);
	for(idx = 0; idx < num; idx++)
	{
		if((res = GMA30x_ReadData(client, prv[idx].raw)))
		{            
			GSE_ERR("read data fail: %d\n", res);
			goto exit;
		}
		
		avg_prv[GMA30x_AXIS_X] += prv[idx].raw[GMA30x_AXIS_X];
		avg_prv[GMA30x_AXIS_Y] += prv[idx].raw[GMA30x_AXIS_Y];
		avg_prv[GMA30x_AXIS_Z] += prv[idx].raw[GMA30x_AXIS_Z];        
		GSE_LOG("[%5d %5d %5d]\n", prv[idx].raw[GMA30x_AXIS_X], prv[idx].raw[GMA30x_AXIS_Y], prv[idx].raw[GMA30x_AXIS_Z]);
	}
	
	avg_prv[GMA30x_AXIS_X] /= num;
	avg_prv[GMA30x_AXIS_Y] /= num;
	avg_prv[GMA30x_AXIS_Z] /= num;    

	/*initial setting for self test*/
	GMA30x_InitSelfTest(client);
	GSE_LOG("SELFTEST:\n");    
	for(idx = 0; idx < num; idx++)
	{
		if((res = GMA30x_ReadData(client, nxt[idx].raw)))
		{            
			GSE_ERR("read data fail: %d\n", res);
			goto exit;
		}
		avg_nxt[GMA30x_AXIS_X] += nxt[idx].raw[GMA30x_AXIS_X];
		avg_nxt[GMA30x_AXIS_Y] += nxt[idx].raw[GMA30x_AXIS_Y];
		avg_nxt[GMA30x_AXIS_Z] += nxt[idx].raw[GMA30x_AXIS_Z];        
		GSE_LOG("[%5d %5d %5d]\n", nxt[idx].raw[GMA30x_AXIS_X], nxt[idx].raw[GMA30x_AXIS_Y], nxt[idx].raw[GMA30x_AXIS_Z]);
	}
	
	avg_nxt[GMA30x_AXIS_X] /= num;
	avg_nxt[GMA30x_AXIS_Y] /= num;
	avg_nxt[GMA30x_AXIS_Z] /= num;    

	GSE_LOG("X: %5d - %5d = %5d \n", avg_nxt[GMA30x_AXIS_X], avg_prv[GMA30x_AXIS_X], avg_nxt[GMA30x_AXIS_X] - avg_prv[GMA30x_AXIS_X]);
	GSE_LOG("Y: %5d - %5d = %5d \n", avg_nxt[GMA30x_AXIS_Y], avg_prv[GMA30x_AXIS_Y], avg_nxt[GMA30x_AXIS_Y] - avg_prv[GMA30x_AXIS_Y]);
	GSE_LOG("Z: %5d - %5d = %5d \n", avg_nxt[GMA30x_AXIS_Z], avg_prv[GMA30x_AXIS_Z], avg_nxt[GMA30x_AXIS_Z] - avg_prv[GMA30x_AXIS_Z]); 
	
	exit:
	/*restore the setting*/    
	gma30x_init_client(client, 0);
	kfree(prv);
	kfree(nxt);
	return count;
}
/*----------------------------------------------------------------------------*/
static ssize_t show_selftest_value(struct device_driver *ddri, char *buf)
{
	struct i2c_client *client = gma30x_i2c_client;
	struct gma30x_i2c_data *obj;

	if(NULL == client)
	{
		GSE_ERR("i2c client is null!!\n");
		return 0;
	}

	obj = i2c_get_clientdata(client);
	return snprintf(buf, PAGE_SIZE, "%d\n", atomic_read(&obj->selftest));
}
/*----------------------------------------------------------------------------*/
static ssize_t store_selftest_value(struct device_driver *ddri, const char *buf, size_t count)
{
	struct gma30x_i2c_data *obj = obj_i2c_data;
	int tmp;

	if(NULL == obj)
	{
		GSE_ERR("i2c data obj is null!!\n");
		return -1;
	}
	
	
	if(1 == sscanf(buf, "%d", &tmp))
	{        
		if(atomic_read(&obj->selftest) && !tmp)
		{
			/*enable -> disable*/
			gma30x_init_client(obj->client, 0);
		}
		else if(!atomic_read(&obj->selftest) && tmp)
		{
			/*disable -> enable*/
			GMA30x_InitSelfTest(obj->client);            
		}
		
		GSE_LOG("selftest: %d => %d\n", atomic_read(&obj->selftest), tmp);
		atomic_set(&obj->selftest, tmp); 
	}
	else
	{ 
		GSE_ERR("invalid content: '%s', length = %d\n", buf, count);   
	}
	return count;
}
/*----------------------------------------------------------------------------*/
static ssize_t show_firlen_value(struct device_driver *ddri, char *buf)
{
#ifdef CONFIG_GMA30x_LOWPASS
	struct i2c_client *client = gma30x_i2c_client;
	struct gma30x_i2c_data *obj = i2c_get_clientdata(client);
	if(atomic_read(&obj->firlen))
	{
		int idx, len = atomic_read(&obj->firlen);
		GSE_LOG("len = %2d, idx = %2d\n", obj->fir.num, obj->fir.idx);

		for(idx = 0; idx < len; idx++)
		{
			GSE_LOG("[%5d %5d %5d]\n", obj->fir.raw[idx][GMA30x_AXIS_X], obj->fir.raw[idx][GMA30x_AXIS_Y], obj->fir.raw[idx][GMA30x_AXIS_Z]);
		}
		
		GSE_LOG("sum = [%5d %5d %5d]\n", obj->fir.sum[GMA30x_AXIS_X], obj->fir.sum[GMA30x_AXIS_Y], obj->fir.sum[GMA30x_AXIS_Z]);
		GSE_LOG("avg = [%5d %5d %5d]\n", obj->fir.sum[GMA30x_AXIS_X]/len, obj->fir.sum[GMA30x_AXIS_Y]/len, obj->fir.sum[GMA30x_AXIS_Z]/len);
	}
	return snprintf(buf, PAGE_SIZE, "%d\n", atomic_read(&obj->firlen));
#else
	return snprintf(buf, PAGE_SIZE, "not support\n");
#endif
}
/*----------------------------------------------------------------------------*/
static ssize_t store_firlen_value(struct device_driver *ddri, const char *buf, size_t count)
{
#ifdef CONFIG_GMA30x_LOWPASS
	struct i2c_client *client = gma30x_i2c_client;  
	struct gma30x_i2c_data *obj = i2c_get_clientdata(client);
	int firlen;

	if(1 != sscanf(buf, "%d", &firlen))
	{
		GSE_ERR("invallid format\n");
	}
	else if(firlen > C_MAX_FIR_LENGTH)
	{
		GSE_ERR("exceeds maximum filter length\n");
	}
	else
	{ 
		atomic_set(&obj->firlen, firlen);
		if(0 == firlen)
		{
			atomic_set(&obj->fir_en, 0);
		}
		else
		{
			memset(&obj->fir, 0x00, sizeof(obj->fir));
			atomic_set(&obj->fir_en, 1);
		}
	}
#endif    
	return count;
}
/*----------------------------------------------------------------------------*/
static ssize_t show_trace_value(struct device_driver *ddri, char *buf)
{
	ssize_t res;
	struct gma30x_i2c_data *obj = obj_i2c_data;
	if (obj == NULL)
	{
		GSE_ERR("i2c_data obj is null!!\n");
		return 0;
	}
	
	res = snprintf(buf, PAGE_SIZE, "0x%04X\n", atomic_read(&obj->trace));     
	return res;    
}
/*----------------------------------------------------------------------------*/
static ssize_t store_trace_value(struct device_driver *ddri, const char *buf, size_t count)
{
	struct gma30x_i2c_data *obj = obj_i2c_data;
	int trace;
	if (obj == NULL)
	{
		GSE_ERR("i2c_data obj is null!!\n");
		return -1;
	}
	
	if(1 == sscanf(buf, "0x%x", &trace))
	{
		atomic_set(&obj->trace, trace);
	}	
	else
	{
		GSE_ERR("invalid content: '%s', length = %d\n", buf, count);
	}
	
	return count;    
}
/*----------------------------------------------------------------------------*/
static ssize_t show_status_value(struct device_driver *ddri, char *buf)
{
	ssize_t len = 0;    
	struct gma30x_i2c_data *obj = obj_i2c_data;
	if (obj == NULL)
	{
		GSE_ERR("i2c_data obj is null!!\n");
		return 0;
	}	
	
	if(obj->hw)
	{
		len += snprintf(buf+len, PAGE_SIZE-len, "CUST: %d %d (%d %d)\n", 
	            obj->hw->i2c_num, obj->hw->direction, obj->hw->power_id, obj->hw->power_vol);   
	}
	else
	{
		len += snprintf(buf+len, PAGE_SIZE-len, "CUST: NULL\n");
	}
	return len;    
}

static ssize_t show_power_status_value(struct device_driver *ddri, char *buf)
{
	int relv = 0;
	if(sensor_power)
		relv = snprintf(buf, PAGE_SIZE, "1\n"); 
	else
		relv = snprintf(buf, PAGE_SIZE, "0\n"); 

	return relv;
}

/*----------------------------------------------------------------------------*/
static DRIVER_ATTR(chipinfo,             S_IRUGO, show_chipinfo_value,      NULL);
static DRIVER_ATTR(sensordata,           S_IRUGO, show_sensordata_value,    NULL);
static DRIVER_ATTR(cali,       S_IWUSR | S_IRUGO, show_cali_value,          store_cali_value);
static DRIVER_ATTR(self,       S_IWUSR | S_IRUGO, show_selftest_value,          store_selftest_value);
static DRIVER_ATTR(selftest,   S_IWUSR | S_IRUGO, show_self_value ,      store_self_value );
static DRIVER_ATTR(firlen,     S_IWUSR | S_IRUGO, show_firlen_value,        store_firlen_value);
static DRIVER_ATTR(trace,      S_IWUSR | S_IRUGO, show_trace_value,         store_trace_value);
static DRIVER_ATTR(status,               S_IRUGO, show_status_value,        NULL);
static DRIVER_ATTR(powerstatus,          S_IRUGO, show_power_status_value,        NULL);

/*----------------------------------------------------------------------------*/
static struct driver_attribute *gma30x_attr_list[] = {
	&driver_attr_chipinfo,     /*chip information*/
	&driver_attr_sensordata,   /*dump sensor data*/
	&driver_attr_cali,         /*show calibration data*/
	&driver_attr_self,         /*self test demo*/
	&driver_attr_selftest,     /*self control: 0: disable, 1: enable*/
	&driver_attr_firlen,       /*filter length: 0: disable, others: enable*/
	&driver_attr_trace,        /*trace log*/
	&driver_attr_status,
	&driver_attr_powerstatus,        
};
/*----------------------------------------------------------------------------*/
static int gma30x_create_attr(struct device_driver *driver) 
{
	int idx, err = 0;
	int num = (int)(sizeof(gma30x_attr_list)/sizeof(gma30x_attr_list[0]));
	if (driver == NULL)
	{
		return -EINVAL;
	}

	for(idx = 0; idx < num; idx++)
	{
		if((err = driver_create_file(driver, gma30x_attr_list[idx])))
		{            
			GSE_ERR("driver_create_file (%s) = %d\n", gma30x_attr_list[idx]->attr.name, err);
			break;
		}
	}    
	return err;
}
/*----------------------------------------------------------------------------*/
static int gma30x_delete_attr(struct device_driver *driver)
{
	int idx ,err = 0;
	int num = (int)(sizeof(gma30x_attr_list)/sizeof(gma30x_attr_list[0]));

	if(driver == NULL)
	{
		return -EINVAL;
	}
	

	for(idx = 0; idx < num; idx++)
	{
		driver_remove_file(driver, gma30x_attr_list[idx]);
	}
	

	return err;
}

/*----------------------------------------------------------------------------*/
int gsensor_operate(void* self, uint32_t command, void* buff_in, int size_in,
		void* buff_out, int size_out, int* actualout)
{
	int err = 0;
	int value, sample_delay;
	struct gma30x_i2c_data *priv = (struct gma30x_i2c_data*)self;
	hwm_sensor_data* gsensor_data;
	char buff[GMA30x_BUFSIZE];
	
	//GSE_FUN(f);
	switch (command)
	{
		case SENSOR_DELAY:
			if((buff_in == NULL) || (size_in < sizeof(int)))
			{
				GSE_ERR("Set delay parameter error!\n");
				err = -EINVAL;
			}
			else
			{
				value = *(int *)buff_in;

				if(value >= 50)
				{
					atomic_set(&priv->filter, 0);
				}
				else
				{					
					priv->fir.num = 0;
					priv->fir.idx = 0;
					priv->fir.sum[GMA30x_AXIS_X] = 0;
					priv->fir.sum[GMA30x_AXIS_Y] = 0;
					priv->fir.sum[GMA30x_AXIS_Z] = 0;
					atomic_set(&priv->filter, 1);
				}
			}
			break;

		case SENSOR_ENABLE:
			if((buff_in == NULL) || (size_in < sizeof(int)))
			{
				GSE_ERR("Enable sensor parameter error!\n");
				err = -EINVAL;
			}
			else
			{
				value = *(int *)buff_in;
				enable_status = (value == 0) ? false : true;
				if(((value == 0) && (sensor_power == false)) ||((value == 1) && (sensor_power == true)))
				{
					GSE_LOG("Gsensor device have updated!\n");
				}
				else
				{
					err = GMA30x_SetPowerMode( priv->client, enable_status);
				}
			}
			break;

		case SENSOR_GET_DATA:
			if((buff_out == NULL) || (size_out< sizeof(hwm_sensor_data)))
			{
				GSE_ERR("get sensor data parameter error!\n");
				err = -EINVAL;
			}
			else
			{
				gsensor_data = (hwm_sensor_data *)buff_out;
				err = GMA30x_ReadSensorData(priv->client, buff, GMA30x_BUFSIZE);
				if(!err)
				{
				   sscanf(buff, "%x %x %x", &gsensor_data->values[0], 
					   &gsensor_data->values[1], &gsensor_data->values[2]);				
				   gsensor_data->status = SENSOR_STATUS_ACCURACY_MEDIUM;				
				   gsensor_data->value_divide = 1000;
				}
			}
			break;
		default:
			GSE_ERR("gsensor operate function no this parameter %d!\n", command);
			err = -1;
			break;
	}
	
	return err;
}
/****************************************************************************** 
 * Function Configuration
******************************************************************************/
static int gma30x_open(struct inode *inode, struct file *file)
{
	file->private_data = gma30x_i2c_client;

	if(file->private_data == NULL)
	{
		GSE_ERR("null pointer!!\n");
		return -EINVAL;
	}
	return nonseekable_open(inode, file);
}
/*----------------------------------------------------------------------------*/
static int gma30x_release(struct inode *inode, struct file *file)
{
	file->private_data = NULL;
	return 0;
}
/*----------------------------------------------------------------------------*/
static long gma30x_unlocked_ioctl(struct file *file, unsigned int cmd,
       unsigned long arg)
{
	struct i2c_client *client = (struct i2c_client*)file->private_data;
	struct gma30x_i2c_data *obj = (struct gma30x_i2c_data*)i2c_get_clientdata(client);	
	char strbuf[GMA30x_BUFSIZE];
	void __user *data;
	SENSOR_DATA sensor_data;
	long err = 0;
	int cali[3];
	//GSE_FUN(f);
	if(_IOC_DIR(cmd) & _IOC_READ)
	{
		err = !access_ok(VERIFY_WRITE, (void __user *)arg, _IOC_SIZE(cmd));
	}
	else if(_IOC_DIR(cmd) & _IOC_WRITE)
	{
		err = !access_ok(VERIFY_READ, (void __user *)arg, _IOC_SIZE(cmd));
	}

	if(err)
	{
		GSE_ERR("access error: %08X, (%2d, %2d)\n", cmd, _IOC_DIR(cmd), _IOC_SIZE(cmd));
		return -EFAULT;
	}

	switch(cmd)
	{
		case GSENSOR_IOCTL_INIT:
			gma30x_init_client(client, 0);			
			break;

		case GSENSOR_IOCTL_READ_CHIPINFO:
			data = (void __user *) arg;
			if(data == NULL)
			{
				err = -EINVAL;
				break;	  
			}
			
			GMA30x_ReadChipInfo(client, strbuf, GMA30x_BUFSIZE);
			GSE_LOG("strbuf = %s\n",strbuf);
			if(copy_to_user(data, strbuf, strlen(strbuf)+1))
			{
				err = -EFAULT;
				break;
			}				 
			break;	  

		case GSENSOR_IOCTL_READ_SENSORDATA:
			data = (void __user *) arg;
			if(data == NULL)
			{
				err = -EINVAL;
				break;	  
			}
			
			GMA30x_ReadSensorData(client, strbuf, GMA30x_BUFSIZE);
			if(copy_to_user(data, strbuf, strlen(strbuf)+1))
			{
				err = -EFAULT;
				break;	  
			}				 
			break;

		case GSENSOR_IOCTL_READ_GAIN:
			data = (void __user *) arg;
			if(data == NULL)
			{
				err = -EINVAL;
				break;	  
			}			
			
			if(copy_to_user(data, &gsensor_gain, sizeof(GSENSOR_VECTOR3D)))
			{
				err = -EFAULT;
				break;
			}				 
			break;

		case GSENSOR_IOCTL_READ_RAW_DATA:
			data = (void __user *) arg;
			if(data == NULL)
			{
				err = -EINVAL;
				break;	  
			}
			GMA30x_ReadRawData(client, strbuf);
			if(copy_to_user(data, strbuf, strlen(strbuf)+1))
			{
				err = -EFAULT;
				break;	  
			}
			break;	  

		case GSENSOR_IOCTL_SET_CALI:
			data = (void __user*)arg;
			if(data == NULL)
			{
				err = -EINVAL;
				break;	  
			}
			if(copy_from_user(&sensor_data, data, sizeof(sensor_data)))
			{
				err = -EFAULT;
				break;	  
			}
			if(atomic_read(&obj->suspend))
			{
				GSE_ERR("Perform calibration in suspend state!!\n");
				err = -EINVAL;
			}
			else
			{
				cali[GMA30x_AXIS_X] = sensor_data.x * obj->reso->sensitivity / GRAVITY_EARTH_1000;
				cali[GMA30x_AXIS_Y] = sensor_data.y * obj->reso->sensitivity / GRAVITY_EARTH_1000;
				cali[GMA30x_AXIS_Z] = sensor_data.z * obj->reso->sensitivity / GRAVITY_EARTH_1000;
				dev_dbg(&obj->client->dev, "cali[0]=%d, sensor_data.x=%d, obj->reso->sensitivity=%d, GRAVITY_EARTH_1000=%d\n", 
						cali[0], sensor_data.x, obj->reso->sensitivity, GRAVITY_EARTH_1000);
				err = GMA30x_WriteCalibration(client, cali);			 
			}
			break;

		case GSENSOR_IOCTL_CLR_CALI:
			err = GMA30x_ResetCalibration(client);
			break;

		case GSENSOR_IOCTL_GET_CALI:
			data = (void __user*)arg;
			if(data == NULL)
			{
				err = -EINVAL;
				break;	  
			}
			if((err = GMA30x_ReadCalibration(client, cali)))
			{
				break;
			}
			
			sensor_data.x = cali[GMA30x_AXIS_X] * GRAVITY_EARTH_1000 / obj->reso->sensitivity;
			sensor_data.y = cali[GMA30x_AXIS_Y] * GRAVITY_EARTH_1000 / obj->reso->sensitivity;
			sensor_data.z = cali[GMA30x_AXIS_Z] * GRAVITY_EARTH_1000 / obj->reso->sensitivity;
			if(copy_to_user(data, &sensor_data, sizeof(sensor_data)))
			{
				err = -EFAULT;
				break;
			}		
			break;
			
		default:
			GSE_ERR("unknown IOCTL: 0x%08x\n", cmd);
			err = -ENOIOCTLCMD;
			break;
			
	}

	return err;
}


/*----------------------------------------------------------------------------*/
static struct file_operations gma30x_fops = {
//	.owner = THIS_MODULE,
	.open = gma30x_open,
	.release = gma30x_release,
	.unlocked_ioctl = gma30x_unlocked_ioctl,
};
/*----------------------------------------------------------------------------*/
static struct miscdevice gma30x_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "gsensor",
	.fops = &gma30x_fops,
};
/*----------------------------------------------------------------------------*/
#ifndef CONFIG_HAS_EARLYSUSPEND
/*----------------------------------------------------------------------------*/
static int gma30x_suspend(struct i2c_client *client, pm_message_t msg) 
{
	struct gma30x_i2c_data *obj = i2c_get_clientdata(client);    
	int err = 0;
	GSE_FUN();    

	if(msg.event == PM_EVENT_SUSPEND)
	{   
		if(obj == NULL)
		{
			GSE_ERR("null pointer!!\n");
			return -EINVAL;
		}
		atomic_set(&obj->suspend, 1);
		if((err = GMA30x_SetPowerMode(obj->client, false)))
		{
			GSE_ERR("write power control fail!!\n");
			return err;
		}        
		GMA302_power(obj->hw, 0);
		GSE_LOG("gma30x_suspend ok\n");
	}
	return err;
}
/*----------------------------------------------------------------------------*/
static int gma30x_resume(struct i2c_client *client)
{
	struct gma30x_i2c_data *obj = i2c_get_clientdata(client);        
	int err;
	GSE_FUN();

	if(obj == NULL)
	{
		dev_err(&client->dev, "null pointer!!\n");
		return -EINVAL;
	}

	GMA302_power(obj->hw, 1);
	if((err = gma30x_init_client(client, 0)))
	{
		dev_err(&client->dev, "initialize client fail!!\n");
		return err;        
	}
	err = GMA30x_SetPowerMode(client, enable_status);
	if(err != GMA30x_SUCCESS)
	{
	    GSE_ERR("set power error\n");
	}
	atomic_set(&obj->suspend, 0);
	dev_dbg(&client->dev, "gma30x_resume ok\n");

	return 0;
}
/*----------------------------------------------------------------------------*/
#else /*CONFIG_HAS_EARLY_SUSPEND is defined*/
/*----------------------------------------------------------------------------*/
static void gma30x_early_suspend(struct early_suspend *h) 
{
	struct gma30x_i2c_data *obj = container_of(h, struct gma30x_i2c_data, early_drv);   
	int err;
	GSE_FUN();    

	if(obj == NULL)
	{
		GSE_ERR("null pointer!!\n");
		return;
	}
	atomic_set(&obj->suspend, 1); 
	if((err = GMA30x_SetPowerMode(obj->client, false)))
	{
		GSE_ERR("write power control fail!!\n");
		return;
	}

	sensor_power = false;
	
	GMA302_power(obj->hw, 0);
}
/*----------------------------------------------------------------------------*/
static void gma30x_late_resume(struct early_suspend *h)
{
	struct gma30x_i2c_data *obj = container_of(h, struct gma30x_i2c_data, early_drv);         
	struct i2c_client *client = gma30x_i2c_client;
	int err;
	GSE_FUN();

	if(obj == NULL)
	{
		GSE_ERR("null pointer!!\n");
		return;
	}

	GMA302_power(obj->hw, 1);
	if((err = gma30x_init_client(obj->client, 0)))
	{
		GSE_ERR("initialize client fail!!\n");
		return;        
	}
	err = GMA30x_SetPowerMode(client, enable_status);
	if(err != GMA30x_SUCCESS)
	{
	    GSE_ERR("set power error\n");
	}
	atomic_set(&obj->suspend, 0);    
}
/*----------------------------------------------------------------------------*/
#endif /*CONFIG_HAS_EARLYSUSPEND*/
/*----------------------------------------------------------------------------*/


static int gma30x_i2c_detect(struct i2c_client *client, int kind, struct i2c_board_info *info) 
{    
	strcpy(info->type, GMA30x_DEV_NAME);
	return 0;
}


/*----------------------------------------------------------------------------*/
static int gma30x_i2c_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	struct i2c_client *new_client;
	struct gma30x_i2c_data *obj;
	struct hwmsen_object sobj;
	int err = 0;
	GSE_FUN();

	if(!(obj = kzalloc(sizeof(*obj), GFP_KERNEL)))
	{
		err = -ENOMEM;
		goto exit;
	}
	
	memset(obj, 0, sizeof(struct gma30x_i2c_data));

	obj->hw = gma302_get_cust_acc_hw(); /*for auto detect feature*/
	
	if((err = hwmsen_get_convert(obj->hw->direction, &obj->cvt)))
	{
		GSE_ERR("invalid direction: %d\n", obj->hw->direction);
		goto exit;
	}

	obj_i2c_data = obj;
	obj->client = client;
	new_client = obj->client;
	i2c_set_clientdata(new_client,obj);
	
	atomic_set(&obj->trace, 0);
	atomic_set(&obj->suspend, 0);
	
#ifdef CONFIG_GMA30x_LOWPASS
	if(obj->hw->firlen > C_MAX_FIR_LENGTH)
	{
		atomic_set(&obj->firlen, C_MAX_FIR_LENGTH);
	}	
	else
	{
		atomic_set(&obj->firlen, obj->hw->firlen);
	}
	
	if(atomic_read(&obj->firlen) > 0)
	{
		atomic_set(&obj->fir_en, 1);
	}
	
#endif

	gma30x_i2c_client = new_client;	

	if((err = gma30x_init_client(new_client, 1)))
	{
		goto exit_init_failed;
	}
	err = GMA30x_SetPowerMode(client, enable_status);
	if(err != GMA30x_SUCCESS)
	{
	    GSE_ERR("set power error\n");
		goto exit_init_failed;
	}

	if((err = misc_register(&gma30x_device)))
	{
		GSE_ERR("gma30x_device register failed\n");
		goto exit_misc_device_register_failed;
	}
#ifdef _GMA302_SUPPORT_AUTO_DETECT_
    if((err = gma30x_create_attr(&gma302_init_info.platform_diver_addr->driver)))
#else
	if((err = gma30x_create_attr(&gma30x_gsensor_driver.driver)))
#endif

	{
		GSE_ERR("create attribute err = %d\n", err);
		goto exit_create_attr_failed;
	}

	sobj.self = obj;
    sobj.polling = 1;
    sobj.sensor_operate = gsensor_operate;
	if((err = hwmsen_attach(ID_ACCELEROMETER, &sobj)))
	{
		GSE_ERR("attach fail = %d\n", err);
		goto exit_kfree;
	}

#ifdef CONFIG_HAS_EARLYSUSPEND
	obj->early_drv.level    = EARLY_SUSPEND_LEVEL_STOP_DRAWING - 2,
	obj->early_drv.suspend  = gma30x_early_suspend,
	obj->early_drv.resume   = gma30x_late_resume,    
	register_early_suspend(&obj->early_drv);
#endif 

	GSE_LOG("%s: OK\n", __func__);    
	return 0;

	exit_create_attr_failed:
	misc_deregister(&gma30x_device);
	exit_misc_device_register_failed:
	exit_init_failed:
	//i2c_detach_client(new_client);
	exit_kfree:
	kfree(obj);
	exit:
	GSE_ERR("%s: err = %d\n", __func__, err);        
	return err;
}

/*----------------------------------------------------------------------------*/
static int gma30x_i2c_remove(struct i2c_client *client)
{
	int err = 0;	
#ifdef _GMA302_SUPPORT_AUTO_DETECT_
    if((err = gma30x_delete_attr(&gma302_init_info.platform_diver_addr->driver)))
#else
	if((err = gma30x_delete_attr(&gma30x_gsensor_driver.driver)))
#endif	
	{
		GSE_ERR("gma30x_delete_attr fail: %d\n", err);
	}
	
	if((err = misc_deregister(&gma30x_device)))
	{
		GSE_ERR("misc_deregister fail: %d\n", err);
	}

#ifndef _GMA303_SUPPORT_AUTO_DETECT_
	hwmsen_detach(ID_ACCELEROMETER);
#endif

	gma30x_i2c_client = NULL;
	i2c_unregister_device(client);
	kfree(i2c_get_clientdata(client));
	return 0;
}
/*----------------------------------------------------------------------------*/
#ifndef _GMA302_SUPPORT_AUTO_DETECT_
static int gma30x_probe(struct platform_device *pdev) 
{
	struct acc_hw *hw = gma302_get_cust_acc_hw();
	GSE_FUN();

	GMA302_power(hw, 1);
	//gma302_force[0] = hw->i2c_num;//modified
	if(i2c_add_driver(&gma30x_i2c_driver))
	{
		GSE_ERR("add driver error\n");
		return -1;
	}
	return 0;
}

/*----------------------------------------------------------------------------*/
static int gma30x_remove(struct platform_device *pdev)
{
    struct acc_hw *hw = gma302_get_cust_acc_hw();

    //GSE_FUN();    
    GMA302_power(hw, 0);    
    i2c_del_driver(&gma30x_i2c_driver);
    return 0;
}
/*----------------------------------------------------------------------------*/
static struct platform_driver gma30x_gsensor_driver = {
	.probe      = gma30x_probe,
	.remove     = gma30x_remove,    
	.driver     = {
		.name  = "gsensor",
//		.owner = THIS_MODULE,
	}
};
#else    // else from _GMA302_SUPPORT_AUTO_DETECT_
/*****************************************
 *** gma30x_remove
 *****************************************/
static int gma30x_remove(void)
{
    struct acc_hw *hw = gma302_get_cust_acc_hw();
    
    GSE_FUN();    

    GMA302_power(hw, 0);    
    i2c_del_driver(&gma30x_i2c_driver);

    return 0;
}

/*****************************************
 *** gma30x_local_init
 *****************************************/
static int  gma30x_local_init(void)
{
    struct acc_hw *hw = gma302_get_cust_acc_hw();
    
    GSE_FUN();    
    GMA302_power(hw, 1);    
  
    if(i2c_add_driver(&gma30x_i2c_driver))
    {
        GSE_ERR("add driver error\n");
        return -1;
    }
    
    return 0;
}
#endif

/*----------------------------------------------------------------------------*/
static int __init gma302_init(void)
{
	//GSE_FUN();
	struct acc_hw *hw = gma302_get_cust_acc_hw();
	GSE_LOG("%s: i2c_number=%d\n", __func__,hw->i2c_num); 
	i2c_register_board_info(hw->i2c_num, &i2c_gma302, 1);
#ifdef _GMA302_SUPPORT_AUTO_DETECT_
    hwmsen_gsensor_add(&gma302_init_info);
#else
	if(platform_driver_register(&gma30x_gsensor_driver))
	{
		GSE_ERR("failed to register driver");
		return -ENODEV;
	}
#endif
	return 0;    
}
/*----------------------------------------------------------------------------*/
static void __exit gma302_exit(void)
{
	GSE_FUN();
#ifndef _GMA302_SUPPORT_AUTO_DETECT_
	platform_driver_unregister(&gma30x_gsensor_driver);
#endif
}
/*----------------------------------------------------------------------------*/
module_init(gma302_init);
module_exit(gma302_exit);
/*----------------------------------------------------------------------------*/
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("GMA30x I2C driver");
MODULE_AUTHOR("Chunlei.Wang@mediatek.com");
