/*
 * crypto-chrdev.c
 *
 * Implementation of character devices
 * for virtio-cryptodev device 
 *
 * Vangelis Koukis <vkoukis@cslab.ece.ntua.gr>
 * Dimitris Siakavaras <jimsiak@cslab.ece.ntua.gr>
 * Stefanos Gerangelos <sgerag@cslab.ece.ntua.gr>
 *
 */
#include <linux/cdev.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/module.h>
#include <linux/wait.h>
#include <linux/virtio.h>
#include <linux/virtio_config.h>

#include "crypto.h"
#include "crypto-chrdev.h"
#include "debug.h"

#include "cryptodev.h"

/*
 * Global data
 */
struct cdev crypto_chrdev_cdev;

/**
 * Given the minor number of the inode return the crypto device 
 * that owns that number.
 **/
static struct crypto_device *get_crypto_dev_by_minor(unsigned int minor)
{
	struct crypto_device *crdev;
	unsigned long flags;

	debug("Entering");

	spin_lock_irqsave(&crdrvdata.lock, flags);         //crdrvdata is a global struct in module.c
	list_for_each_entry(crdev, &crdrvdata.devs, list) {
		if (crdev->minor == minor)
			goto out;
	}
	crdev = NULL;

out:
	spin_unlock_irqrestore(&crdrvdata.lock, flags);

	debug("Leaving");
	return crdev;
}

/*************************************
 * Implementation of file operations
 * for the Crypto character device
 *************************************/

static int crypto_chrdev_open(struct inode *inode, struct file *filp)
{
	int ret = 0;
	int err;
	unsigned int len;
	struct crypto_open_file *crof; //crypto open file
	struct crypto_device *crdev;
	unsigned int *syscall_type;
	// int *host_fd;
	struct scatterlist syscall_type_sg, host_fd_sg, *sgs[2];
	unsigned int num_out = 0, num_in = 0;
	struct virtqueue *vq;
	unsigned long flags;

	debug("Entering");

	syscall_type = kzalloc(sizeof(*syscall_type), GFP_KERNEL);
	*syscall_type = VIRTIO_CRYPTODEV_SYSCALL_OPEN;
	// host_fd = kzalloc(sizeof(*host_fd), GFP_KERNEL);
	// *host_fd = -1;

	ret = -ENODEV;
	if ((ret = nonseekable_open(inode, filp)) < 0)
		goto fail;

	/* Associate this open file with the relevant crypto device. */
	crdev = get_crypto_dev_by_minor(iminor(inode));
	if (!crdev) {
		debug("Could not find crypto device with %u minor", 
		      iminor(inode));
		ret = -ENODEV;
		goto fail;
	}

	crof = kzalloc(sizeof(*crof), GFP_KERNEL);
	if (!crof) {
		ret = -ENOMEM;
		goto fail;
	}
	crof->crdev = crdev;
	crof->host_fd = -1;
	filp->private_data = crof;
	vq = crdev->vq;

	/**
	 * We need two sg lists, one for syscall_type and one to get the 
	 * file descriptor from the host.
	 **/

 	//static inline void sg_init_one(struct scatterlist *sg, const void *buf, unsigned int buflen)
	sg_init_one(&syscall_type_sg, syscall_type, sizeof(*syscall_type));
	sgs[num_out++] = &syscall_type_sg;
	sg_init_one(&host_fd_sg, &(crof->host_fd), sizeof(crof->host_fd));
	sgs[num_out + num_in++] = &host_fd_sg;

	spin_lock_irqsave(&crdev->lock, flags); //lock crypto device

	//int virtqueue_add_sgs(struct virtqueue *vq, struct scatterlist *sgs[], unsigned int out_sgs, unsigned int in_sgs, void *data, gfp_t gfp);
	err = virtqueue_add_sgs(vq, sgs, num_out, num_in, &syscall_type_sg, GFP_ATOMIC);
	virtqueue_kick(vq);

	/**
	 * Wait for the host to process our data.
	 **/
	while (virtqueue_get_buf(vq, &len) == NULL); // do nothing while waiting

	spin_unlock_irqrestore(&crdev->lock, flags); //unlock crypto device

	/* If host failed to open() return -ENODEV. */
	if(crof->host_fd < 0){
		debug("Host failed to open(). Leaving");
		return -ENODEV;
	}

fail:
	kfree(syscall_type);
	debug("Leaving");
	return ret;
}

static int crypto_chrdev_release(struct inode *inode, struct file *filp)
{
	int ret = 0, err;
	unsigned int len;
	struct crypto_open_file *crof = filp->private_data;
	struct crypto_device *crdev = crof->crdev;
	unsigned int *syscall_type;
	struct scatterlist syscall_type_sg, host_fd_sg, *sgs[2];
	unsigned int num_out = 0, num_in = 0;
	struct virtqueue *vq = crdev->vq;
	unsigned long flags;

	debug("Entering");

	syscall_type = kzalloc(sizeof(*syscall_type), GFP_KERNEL);
	*syscall_type = VIRTIO_CRYPTODEV_SYSCALL_CLOSE;

	sg_init_one(&syscall_type_sg, syscall_type, sizeof(*syscall_type));
	sgs[num_out++] = &syscall_type_sg;
	sg_init_one(&host_fd_sg, &(crof->host_fd), sizeof(crof->host_fd));
	sgs[num_out++] = &host_fd_sg;

	/**
	 * Send data to the host.
	 **/
	spin_lock_irqsave(&crdev->lock, flags); //lock crypto device

	err = virtqueue_add_sgs(vq, sgs, num_out, num_in, &syscall_type_sg, GFP_ATOMIC);
	virtqueue_kick(vq);

	/**
	 * Wait for the host to process our data.
	 **/
	while (virtqueue_get_buf(vq, &len) == NULL); // do nothing while waiting

	spin_unlock_irqrestore(&crdev->lock, flags); //unlock crypto device

	kfree(syscall_type);
	kfree(crof);
	debug("Leaving");
	return ret;

}

static long crypto_chrdev_ioctl(struct file *filp, unsigned int cmd, 
                                unsigned long arg)
{
	long ret = 0;
	int err;
	struct crypto_open_file *crof = filp->private_data;
	struct crypto_device *crdev = crof->crdev;
	struct virtqueue *vq = crdev->vq;
	struct scatterlist syscall_type_sg, output_msg_sg, input_msg_sg, host_fd_sg, cmd_sg, session_sg, sess_ses_sg, session_key_sg, crypt_sg, crypto_src_sg, crypto_dst_sg, crypto_iv_sg, *sgs[9];
	unsigned int num_out, num_in, len;
#define MSG_LEN 100
	unsigned char *output_msg, *input_msg;
	unsigned int *syscall_type;
	unsigned long flags;
	struct session_op *user_session, copied_session;
	struct crypt_op *user_crypt, copied_crypt;
	__u8 *session_key = NULL, *crypto_src = NULL, *crypto_dst = NULL, *crypto_iv = NULL;
	__u32 *user_sess_ses, *copied_sess_ses = NULL;

	debug("Entering");

	/**
	 * Allocate all data that will be sent to the host.
	 **/
	output_msg = kzalloc(MSG_LEN, GFP_KERNEL);
	input_msg = kzalloc(MSG_LEN, GFP_KERNEL);
	syscall_type = kzalloc(sizeof(*syscall_type), GFP_KERNEL);
	*syscall_type = VIRTIO_CRYPTODEV_SYSCALL_IOCTL;

	num_out = 0;
	num_in = 0;

	/**
	 *  These are common to all ioctl commands.
	 **/
	sg_init_one(&syscall_type_sg, syscall_type, sizeof(*syscall_type));
	sgs[num_out++] = &syscall_type_sg;
	sg_init_one(&host_fd_sg, &(crof->host_fd), sizeof(crof->host_fd));
	sgs[num_out++] = &host_fd_sg;
	sg_init_one(&cmd_sg, &cmd, sizeof(cmd));
	sgs[num_out++] = &cmd_sg;

	/**
	 *  Add all the cmd specific sg lists.
	 **/
	switch (cmd) {
	case CIOCGSESSION:
		debug("CIOCGSESSION");
		memcpy(output_msg, "Hello HOST from ioctl CIOCGSESSION.", 36);
		input_msg[0] = '\0';
		sg_init_one(&output_msg_sg, output_msg, MSG_LEN);
		sgs[num_out++] = &output_msg_sg;
		sg_init_one(&input_msg_sg, input_msg, MSG_LEN);
		sgs[num_out + num_in++] = &input_msg_sg;

		user_session = (struct session_op *)arg;

		if((ret = copy_from_user(&copied_session, user_session, sizeof(struct session_op)))) {
			debug("Failed to copy_from_user (copied_session).");
			goto fail;
		}

		debug("after copy_from_user(&copied_session, user_session, sizeof(struct session_op))");

		session_key = kzalloc(copied_session.keylen+1, GFP_KERNEL);
		if((ret = copy_from_user(session_key, user_session->key, (user_session->keylen)*sizeof(__u8)))){
			debug("Failed to copy_from_user (session_key).");
			goto fail;
		}
		session_key[copied_session.keylen]='\0'; // ensure null char at the end of session_key
		// copied_session.key = session_key;

		// we could also do the same for mackeylen - mackey, but we don't use it

		sg_init_one(&session_sg, &copied_session, sizeof(struct session_op));
		sgs[num_out + num_in++] = &session_sg;
		debug("after sgs[num_out + num_in++] = &session_sg;");
		// sg_init_one(&session_key_sg, session_key, copied_session.keylen+1);
		// sgs[num_out + num_in++] = &session_key_sg;

		break;

	case CIOCFSESSION:
		debug("CIOCFSESSION");
		memcpy(output_msg, "Hello HOST from ioctl CIOCFSESSION.", 36);
		input_msg[0] = '\0';
		sg_init_one(&output_msg_sg, output_msg, MSG_LEN);
		sgs[num_out++] = &output_msg_sg;
		sg_init_one(&input_msg_sg, input_msg, MSG_LEN);
		sgs[num_out + num_in++] = &input_msg_sg;

		// user_sess_ses = (__u32 *)arg;

		// copied_sess_ses = kzalloc(sizeof(__u32), GFP_KERNEL);
		// if((ret = copy_from_user(copied_sess_ses, user_sess_ses, sizeof(__u32)))){
		// 	debug("Failed to copy_from_user (copied_sess_ses).");
		// 	goto fail;
		// }

		// sg_init_one(&sess_ses_sg, copied_sess_ses, sizeof(__u32));
		// sgs[num_out + num_in++] = &sess_ses_sg;

		break;

	case CIOCCRYPT:
		debug("CIOCCRYPT");
		memcpy(output_msg, "Hello HOST from ioctl CIOCCRYPT.", 33);
		input_msg[0] = '\0';
		sg_init_one(&output_msg_sg, output_msg, MSG_LEN);
		sgs[num_out++] = &output_msg_sg;
		sg_init_one(&input_msg_sg, input_msg, MSG_LEN);
		sgs[num_out + num_in++] = &input_msg_sg;

		// user_crypt = (struct crypt_op *)arg;

		// if((ret = copy_from_user(&copied_crypt, user_crypt, sizeof(struct crypt_op)))) {
		// 	debug("Failed to copy_from_user (copied_crypt).");
		// 	goto fail;
		// }

		// crypto_src = kzalloc(copied_crypt.len * sizeof(__u8), GFP_KERNEL);
		// if((ret = copy_from_user(crypto_src, user_crypt->src, copied_crypt.len * sizeof(__u8)))) {
		// 	debug("Failed to copy_from_user (crypto_src).");
		// 	goto fail;
		// }
		// copied_crypt.src = crypto_src;

		// crypto_dst = kzalloc(copied_crypt.len * sizeof(__u8), GFP_KERNEL);
		// copied_crypt.dst = crypto_dst;

		// crypto_iv = kzalloc(16 * sizeof(__u8), GFP_KERNEL);
		// if((ret = copy_from_user(crypto_iv, user_crypt->iv, 16 * sizeof(__u8)))) {
		// 	debug("Failed to copy_from_user (crypto_iv).");
		// 	goto fail;
		// }
		// copied_crypt.iv = crypto_iv;

		// sg_init_one(&crypt_sg, &copied_crypt, sizeof(struct crypt_op));
		// sgs[num_out++] = &crypt_sg;
		// sg_init_one(&crypto_src_sg, crypto_src, copied_crypt.len * sizeof(__u8));
		// sgs[num_out++] = &crypto_src_sg;
		// sg_init_one(&crypto_dst_sg, crypto_dst, copied_crypt.len * sizeof(__u8));
		// sgs[num_out + num_in++] = &crypto_dst_sg;
		// sg_init_one(&crypto_iv_sg, crypto_iv, 16 * sizeof(__u8));
		// sgs[num_out++] = &crypto_iv_sg;

		break;

	default:
		debug("Unsupported ioctl command");

		break;
	}


	/**
	 * Wait for the host to process our data.
	 **/
	spin_lock_irqsave(&crdev->lock, flags);
	err = virtqueue_add_sgs(vq, sgs, num_out, num_in,
	                        &syscall_type_sg, GFP_ATOMIC);
	virtqueue_kick(vq);
	while (virtqueue_get_buf(vq, &len) == NULL)
		/* do nothing */;
	spin_unlock_irqrestore(&crdev->lock, flags);

	debug("We said: '%s'", output_msg);
	debug("Host answered: '%s'", input_msg);

fail:
	switch (cmd) {
		case CIOCGSESSION:
			kfree(session_key);
			break;

		case CIOCFSESSION:
			kfree(copied_sess_ses);
			break;

		case CIOCCRYPT:
			kfree(crypto_src);
			kfree(crypto_dst);
			kfree(crypto_iv);
			break;
	}

	kfree(output_msg);
	kfree(input_msg);
	kfree(syscall_type);

	debug("Leaving");

	return ret;
}

static ssize_t crypto_chrdev_read(struct file *filp, char __user *usrbuf, 
                                  size_t cnt, loff_t *f_pos)
{
	debug("Entering");
	debug("Leaving");
	return -EINVAL;
}

static struct file_operations crypto_chrdev_fops = 
{
	.owner          = THIS_MODULE,
	.open           = crypto_chrdev_open,
	.release        = crypto_chrdev_release,
	.read           = crypto_chrdev_read,
	.unlocked_ioctl = crypto_chrdev_ioctl,
};

int crypto_chrdev_init(void)
{
	int ret;
	dev_t dev_no;
	unsigned int crypto_minor_cnt = CRYPTO_NR_DEVICES;
	
	debug("Initializing character device...");
	cdev_init(&crypto_chrdev_cdev, &crypto_chrdev_fops);
	crypto_chrdev_cdev.owner = THIS_MODULE;
	
	dev_no = MKDEV(CRYPTO_CHRDEV_MAJOR, 0);
	ret = register_chrdev_region(dev_no, crypto_minor_cnt, "crypto_devs");
	if (ret < 0) {
		debug("failed to register region, ret = %d", ret);
		goto out;
	}
	ret = cdev_add(&crypto_chrdev_cdev, dev_no, crypto_minor_cnt);
	if (ret < 0) {
		debug("failed to add character device");
		goto out_with_chrdev_region;
	}

	debug("Completed successfully");
	return 0;

out_with_chrdev_region:
	unregister_chrdev_region(dev_no, crypto_minor_cnt);
out:
	return ret;
}

void crypto_chrdev_destroy(void)
{
	dev_t dev_no;
	unsigned int crypto_minor_cnt = CRYPTO_NR_DEVICES;

	debug("entering");
	dev_no = MKDEV(CRYPTO_CHRDEV_MAJOR, 0);
	cdev_del(&crypto_chrdev_cdev);
	unregister_chrdev_region(dev_no, crypto_minor_cnt);
	debug("leaving");
}
