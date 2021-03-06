/* 		
 *			Модуль блочного устройства, создание сырого устройства /dev/xxx 
 *				с возможностью выполнения на нем блочных операций 
 */

#include <linux/genhd.h>
#include <linux/errno.h>
#include <linux/hdreg.h>
#include <linux/module.h>
#include <linux/blkdev.h>
#include <linux/vmalloc.h>
#include <linux/version.h>

MODULE_VERSION( "1.0" );
MODULE_LICENSE( "GPL" );
MODULE_AUTHOR( "Vecnik88" );
MODULE_DESCRIPTION( "Block device train" );

#define DEV_MINORS 16							// <---. кол-во разделов на диске( не всегда играет роль, их можно сделать не больше определенного кол-ва )
#define MY_DEVICE_NAME "xd"						// <---. родовое имя устройства( sda, sdb и тд )
#define KERNEL_SECTOR_SIZE 512					// <---. размер сектора

/* макросы для удобства и краткости сообщений в ядро */
#define LOG(...) printk( KERN_INFO __VA_ARGS__ )
#define ERR(...) printk( KERN_ERR "ERROR " __VA_ARGS__ )
#define DBG(...) if( debug > 0 ) printk( KERN_DEBUG "# " __VA_ARGS__ )

#define blk_fs_request(rq)      ((rq)->cmd_type == REQ_TYPE_FS)

char buffer[4*1024*1024];

static int diskmb = 4;									// <---. размер диска в Mb
module_param_named( size, diskmb, int, 0 );				

static int debug = 0;									// <---. для отладки
module_param( debug, int, 0 );							

static int major = 0;									// <---. старший номер устройства
module_param( major, int, 0 );

static int hardsect_size = KERNEL_SECTOR_SIZE;			// <---. размер одного сектора
module_param( hardsect_size, int, 0 );

static int ndevices = 4;								// <---. кол-во разделов( они же minors )
module_param( ndevices, int, 0 );

static int nsectors;

struct disk_dev{
	int size;
	u8 * data;
	spinlock_t lock;
	struct request_queue * queue;
	struct gendisk * gd;
};

static struct disk_dev* Devices = NULL;

static int transfer( struct disk_dev* dev, unsigned long sector,
					 unsigned long nsect, char* buffer, int write ){
	
	unsigned long offset = sector * KERNEL_SECTOR_SIZE;
	unsigned long nbytes = nsect * KERNEL_SECTOR_SIZE;

	if( ( offset + nbytes ) > dev->size ){
		ERR( "beyound-end write (%ld %ld)\n", offset, nbytes );
		return -EIO;
	}
	if( write )
		memcpy( dev->data + offset, buffer, nbytes );
	else
		memcpy( buffer, dev->data + offset, nbytes );

	return 0;
}

static void simple_request( struct request_queue * q ){
	struct request* req;
	unsigned nr_sectors, sector;
	DBG( "entering simple request routine\n" );
	req = blk_fetch_request( q );
	while( req ){
		int ret = 0;
		struct disk_dev* dev = req->rq_disk->private_data;
		if( !( blk_fs_request( req ) ) ) {
			ERR( "skip non-fs request\n" );
			__blk_end_request_all( req, -EIO );
			req = blk_fetch_request( q );
			continue;
		}
		nr_sectors = blk_rq_cur_sectors( req );
		sector = blk_rq_pos( req );
		ret = transfer( dev, sector, nr_sectors, buffer, rq_data_dir( req ) );
		if( !__blk_end_request_cur( req, ret ) )
			req = blk_fetch_request( q );
	}
}

static int my_getgeo( struct block_device *bdev, struct hd_geometry *geo ) {
   unsigned long sectors = ( diskmb * 1024 ) * 2;
   DBG( KERN_INFO "getgeo\n" );
   geo->heads = 4;
   geo->sectors = 16;
   geo->cylinders = sectors / geo->heads / geo->sectors;
   geo->start = geo->sectors;

   return 0;
};

static int my_ioctl( struct block_device *bdev, fmode_t mode,
                     unsigned int cmd, unsigned long arg ) {
   LOG( "ioctl cmd=%X\n", cmd );
   switch( cmd ) {
      case HDIO_GETGEO: {
         struct hd_geometry geo;
         LOG( "ioctk HDIO_GETGEO\n" );
         my_getgeo( bdev, &geo );
         if( copy_to_user( (void __user *)arg, &geo, sizeof( geo ) ) )
            return -EFAULT;
         return 0;
      }
      default:
         ERR( "ioctl unknown command\n" );
         return -ENOTTY;
   }
}

static struct block_device_operations mybdrv_fops = {
	.owner = THIS_MODULE,
	.getgeo = my_getgeo
};

static void setup_device( struct disk_dev* dev, int which ){
	memset( dev, 0, sizeof( struct disk_dev ) );
	dev->size = diskmb * 1024 * 1024;
	dev->data = vmalloc( dev->size );
	if( dev->data == NULL ){
		ERR( "vmalloc failure. \n" );
		return;
	}

	spin_lock_init( &dev->lock );
	
	dev->queue = blk_init_queue( simple_request, &dev->lock );
	if( dev->queue == NULL )
		goto out_vfree;

	blk_queue_logical_block_size( dev->queue, hardsect_size );
	dev->queue->queuedata = dev;
	dev->gd = alloc_disk( DEV_MINORS );							// <---. число разделов при разбиении
	if( ! dev->gd ){
		ERR( "allock_disk failure\n" );
		goto out_vfree;
	}

	dev->gd->major = major;
	dev->gd->minors = DEV_MINORS;
	dev->gd->first_minor = which * DEV_MINORS;
	dev->gd->fops = &mybdrv_fops;
	dev->gd->queue = dev->queue;
	dev->gd->private_data = dev;
	snprintf( dev->gd->disk_name, 32, MY_DEVICE_NAME"%c", which + 'a' );
	set_capacity( dev->gd, nsectors * ( hardsect_size / KERNEL_SECTOR_SIZE ) );
	add_disk( dev->gd );
	return;

out_vfree:
	if( dev->data )
		vfree( dev->data );
}

static int __init blk_init( void ){
	int i = 0;
	nsectors = diskmb * 1024 * 1024 / hardsect_size;
	major = register_blkdev( major, MY_DEVICE_NAME );

	if( major <= 0 ){
		ERR( "unable to get major number\n" );
		return -EBUSY;
	}

	Devices = kmalloc( ndevices * sizeof( struct disk_dev ), GFP_KERNEL );
	if( Devices == NULL )
		goto out_unregister;

	for( i = 0; i < ndevices; ++i ){
		setup_device( Devices + i, i );
	}

	return 0;

out_unregister:
	unregister_blkdev( major, MY_DEVICE_NAME );
	return -ENOMEM;
}

static void __exit blk_exit( void ){
	int i = 0;
	for( i = 0; i < ndevices; ++i ){
		struct disk_dev* dev = Devices + i;
		if( dev->gd ){
			del_gendisk( dev->gd );
			put_disk( dev->gd );
		}

		blk_cleanup_queue( dev->queue );
		
		if( dev->data )
			vfree( dev->data );
	}

	unregister_blkdev( major, MY_DEVICE_NAME );
	kfree( Devices );
}

module_init( blk_init );
module_exit( blk_exit );

