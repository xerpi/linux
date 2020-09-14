/*
 * nintendo3ds_mmc.c
 *
 *  Copyright (C) 2016 Sergi Granell <xerpi.g.12@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/sched.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/of.h>
#include <linux/types.h>
#include <linux/genhd.h>
#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/errno.h>

#include <mach/pxi.h>

#define NINTENDO3DS_MMC_BLOCKSIZE   512
#define NINTENDO3DS_MMC_FIRST_MINOR   0
#define NINTENDO3DS_MMC_MINOR_CNT    16

/****************************************************************************/

struct nintendo3ds_mmc {
	int major;
	sector_t capacity;  /* in sectors */
	spinlock_t lock;
	struct blk_mq_tag_set tag_set;
	struct request_queue *queue;
	struct gendisk *disk;

};

static void nintendo3ds_pxi_mmc_write_sectors(struct nintendo3ds_mmc *mmc,
	sector_t sector_off, u8 *buffer, unsigned int sectors)
{
	/*memcpy(dev_data + sector_off * NINTENDO3DS_MMC_BLOCKSIZE, buffer,
		sectors * NINTENDO3DS_MMC_BLOCKSIZE);*/
}

static void nintendo3ds_pxi_mmc_read_sectors(struct nintendo3ds_mmc *mmc,
	sector_t sector_off, const u8 *buffer, unsigned int sectors)
{
	unsigned int i;

	for (i = 0; i < sectors; i++) {
		struct pxi_cmd_sdmmc_read_sector cmd = {
			.header.cmd = PXI_CMD_SDMMC_READ_SECTOR,
			.header.len = sizeof(cmd) - sizeof(struct pxi_cmd_hdr),
			.sector = sector_off + i,
			.paddr = (u32)virt_to_phys(buffer + i * NINTENDO3DS_MMC_BLOCKSIZE)
		};

		pxi_send_cmd((struct pxi_cmd_hdr *)&cmd);
	}
}

static int nintendo3ds_mmc_xfer_request(struct nintendo3ds_mmc *mmc, struct request *req,
					unsigned int *nr_bytes)
{
	struct bio_vec bvec;
	struct req_iterator iter;
	sector_t sector_offset;
	unsigned int sectors;
	u8 *buffer;
	int ret = 0;
	const int dir = rq_data_dir(req);
	const sector_t start_sector = blk_rq_pos(req);
	const unsigned int sector_cnt = blk_rq_sectors(req);

	sector_offset = 0;
	*nr_bytes = 0;

	rq_for_each_segment(bvec, req, iter) {
		buffer = page_address(bvec.bv_page) + bvec.bv_offset;
		sectors = bvec.bv_len / NINTENDO3DS_MMC_BLOCKSIZE;

		/*printk("n3ds MMC: start sect: %lld, sect off: %lld; buffer: %p; num sects: %u\n",
			start_sector, sector_offset, buffer, sectors);*/

		if (dir == READ) {
			nintendo3ds_pxi_mmc_read_sectors(mmc, start_sector + sector_offset,
							 buffer, sectors);
		} else {
			nintendo3ds_pxi_mmc_write_sectors(mmc, start_sector + sector_offset,
							  buffer, sectors);
		}

		sector_offset += sectors;
		*nr_bytes += bvec.bv_len;
	}

	if (sector_offset != sector_cnt) {
		printk(KERN_ERR "n3ds MMC: bio info doesn't match with the request info");
		ret = -EIO;
	}

	return ret;
}

static blk_status_t nintendo3ds_mmc_mq_queue_rq(struct blk_mq_hw_ctx *hctx,
						const struct blk_mq_queue_data* bd)
{
	struct request *rq = bd->rq;
	struct nintendo3ds_mmc *mmc = rq->rq_disk->private_data;
	blk_status_t status = BLK_STS_OK;
	unsigned int nr_bytes;

	blk_mq_start_request(rq);

	if (nintendo3ds_mmc_xfer_request(mmc, rq, &nr_bytes) != 0)
		status = BLK_STS_IOERR;

	if (blk_update_request(rq, status, nr_bytes))
		BUG();

	__blk_mq_end_request(rq, status);

	return status;
}


static int nintendo3ds_mmc_open(struct block_device *bdev, fmode_t mode)
{
	unsigned unit = iminor(bdev->bd_inode);

	if (unit > NINTENDO3DS_MMC_MINOR_CNT)
		return -ENODEV;

	return 0;
}

static void nintendo3ds_mmc_release(struct gendisk *disk, fmode_t mode)
{

}

static struct block_device_operations nintendo3ds_mmc_fops = {
	.owner = THIS_MODULE,
	.open = nintendo3ds_mmc_open,
	.release = nintendo3ds_mmc_release,
};

static struct blk_mq_ops nintendo3ds_mmc_mq_ops = {
	.queue_rq = nintendo3ds_mmc_mq_queue_rq,
};

static int nintendo3ds_mmc_probe(struct platform_device *pdev)
{
	int error;
	struct nintendo3ds_mmc *mmc;

	mmc = kzalloc(sizeof(*mmc), GFP_KERNEL);
	if (!mmc)
		return -ENOMEM;

	mmc->major = register_blkdev(0, "nintendo3ds_mmc");
	if (mmc->major <= 0) {
		error = -EBUSY;
		goto error_reg_blkdev;
	}

	//mmc->size = pxi_...
	mmc->capacity = 31586304; /* 16GB SD card */

	printk("nintendo3ds_mmc: capacity: %lld\n", mmc->capacity);
	printk("nintendo3ds_mmc: major: %d\n", mmc->major);

	spin_lock_init(&mmc->lock);

	mmc->queue = blk_mq_init_sq_queue(&mmc->tag_set, &nintendo3ds_mmc_mq_ops, 128,
					  BLK_MQ_F_SHOULD_MERGE);
	if (!mmc->queue) {
		error = -ENOMEM;
		goto error_init_queue;
	}

	mmc->queue->queuedata = mmc;

	mmc->disk = alloc_disk(NINTENDO3DS_MMC_MINOR_CNT);
	if (!mmc->disk) {
		error = -ENOMEM;
		goto error_alloc_disk;
	}

	mmc->disk->major = mmc->major;
	mmc->disk->first_minor = NINTENDO3DS_MMC_FIRST_MINOR;
	mmc->disk->fops = &nintendo3ds_mmc_fops;
	mmc->disk->private_data = mmc;
	mmc->disk->queue = mmc->queue;
	sprintf(mmc->disk->disk_name, "nintendo3ds_mmc");

	platform_set_drvdata(pdev, mmc);

	set_capacity(mmc->disk, mmc->capacity);
	add_disk(mmc->disk);

	dev_info(&pdev->dev, "Nintendo 3ds PXI SD/MMC %d\n", mmc->major);

	return 0;

error_alloc_disk:
	blk_cleanup_queue(mmc->queue);
error_init_queue:
	unregister_blkdev(mmc->major, "nintendo3ds_mmc");
error_reg_blkdev:
	kfree(mmc);

	return error;
}

static int __exit nintendo3ds_mmc_remove(struct platform_device *pdev)
{
	struct nintendo3ds_mmc *mmc = platform_get_drvdata(pdev);

	if (mmc) {
		del_gendisk(mmc->disk);
		put_disk(mmc->disk);
		blk_cleanup_queue(mmc->queue);
		unregister_blkdev(mmc->major, "nintendo3ds_mmc");
	}

	return 0;
}

static const struct of_device_id nintendo3ds_mmc_dt_ids[] = {
	{ .compatible = "nintendo3ds-mmc", },
	{},
};
MODULE_DEVICE_TABLE(of, nintendo3ds_mmc_dt_ids);

static struct platform_driver nintendo3ds_mmc_driver = {
	.driver		= {
		.name	= "nintendo3ds_mmc",
		.owner = THIS_MODULE,
		.of_match_table = nintendo3ds_mmc_dt_ids,
	},
	.remove		= __exit_p(nintendo3ds_mmc_remove),
};

module_platform_driver_probe(nintendo3ds_mmc_driver, nintendo3ds_mmc_probe);

MODULE_AUTHOR("Sergi Granell");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Nintendo 3DS PXI SD/MMC driver");
MODULE_ALIAS("platform:nintendo3ds_mmc");
