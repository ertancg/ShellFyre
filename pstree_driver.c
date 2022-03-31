#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kdev_t.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/ioctl.h>
#include <linux/sched.h>

#define IOCTL_MODE_READ _IOW('p', 0, char*)
#define IOCTL_PID_READ _IOW('p', 1, int32_t*)

int32_t pid;
char mode[8];

dev_t dev = 0;

struct queue_entry{
	char name[128];
	int id;
	struct list_head lst;
};

static LIST_HEAD(task_queue);

static struct class *dev_class;
static struct cdev my_cdev;

static int __init pstree_driver_init(void);
static void __exit pstree_driver_exit(void);
static int pstree_open(struct inode *inode, struct file *file);
static int pstree_release(struct inode *inode, struct file *file);
static ssize_t pstree_read(struct file *filp, char __user *buf, size_t len, loff_t* off);
static ssize_t pstree_write(struct file *filp, const char __user *buf, size_t len, loff_t* off);
static long pstree_ioctl(struct file *file, unsigned int cmd, unsigned long arg);
static void process_command(void);
static void bfs(struct task_struct *task);
static void bfs_initiate(struct task_struct *task);
static void print_queue(void);

static struct file_operations fops = 
{
	.owner	= THIS_MODULE,
	.read = pstree_read,
	.write = pstree_write,
	.open = pstree_open,
	.release = pstree_release,
	.unlocked_ioctl = pstree_ioctl,
};

static int pstree_open(struct inode *inode, struct file *file){
	printk(KERN_INFO"Module opened successfully. \n");
	return 0;
}


static int pstree_release(struct inode *inode, struct file *file){
	printk(KERN_INFO"Module released successfully.\n");
	return 0;
}

static long pstree_ioctl(struct file *file, unsigned int cmd, unsigned long arg){
	switch(cmd){
		case IOCTL_PID_READ:
			if(copy_from_user(&pid , (int32_t *)arg, sizeof(pid))){
				printk(KERN_INFO"Couldn't read pid from the user space");
				break;
			}else{
				printk(KERN_INFO"Read succesfull: %d", pid);
				process_command();
				break;
			}
		case IOCTL_MODE_READ:
			if(copy_from_user(mode , (char *)arg, sizeof(mode))){
				printk(KERN_INFO"Couldn't read mode from the user space");
				break;
			}else{
				printk(KERN_INFO"Read succesfull: %s", mode);
				break;
			}
		default:
			printk(KERN_INFO"No read or write. \n");
			break;
	}
	return 0;
}
static void process_command(){
	if(strcmp(mode, "-d") == 0){
		if((int)pid == 1234){
			printk(KERN_INFO"depth first on pid: %d", pid);
		}
	}
	if(strcmp(mode, "-b") == 0){
		bfs_initiate(&init_task);
	}
}

static void bfs_initiate(struct task_struct *task){
	INIT_LIST_HEAD(&task_queue);
	for_each_process(task){
		if((int) pid == task->pid){
			struct queue_entry *head = kmalloc(sizeof(*head), GFP_KERNEL); 
			strcpy(head->name, task->comm);
			head->id = task->pid;
			list_add_tail(&head->lst, &task_queue);
			bfs(task);
			print_queue();
			break;
		}
	}
}

static void bfs(struct task_struct *task){
	struct task_struct *current_task;
	struct list_head *list;
	struct queue_entry *new;

	list_for_each(list, &task->children) {
    	current_task = list_entry(list, struct task_struct, sibling);
    	
    	/* task now points to one of current's children */ 
    	printk(KERN_INFO"name: %s[%d]\n", current_task->comm, current_task->pid);
    	new = kmalloc(sizeof(*new), GFP_KERNEL);
    	strcpy(new->name, task->comm);
    	new->id = task->pid;
    	INIT_LIST_HEAD(&new->lst);
 
    	list_add_tail(&new->lst, &task_queue);
    	bfs(current_task);
    }
}

static void print_queue(){
	struct queue_entry *current_entry;
	struct list_head *list;
	list_for_each(list, &task_queue){
		current_entry = list_entry(list, struct queue_entry, lst);
		printk(KERN_INFO"QUEUE READ = name: %s, id: %d", current_entry->name, current_entry->id);
	}
}

static ssize_t pstree_write(struct file *filp, const char __user *buf, size_t len, loff_t* off){
	return len;
}

static ssize_t pstree_read(struct file *filp, char __user *buf, size_t len, loff_t* off){
	return len;
}

static int __init pstree_driver_init(void){
	if((alloc_chrdev_region(&dev, 0, 1, "pstree_driver")) < 0){
		printk(KERN_INFO"Cannot allocate the major number...\n");
	}

	printk(KERN_INFO"pstree Major = %d, Minor = %d..\n", MAJOR(dev), MINOR(dev));

	cdev_init(&my_cdev, &fops);

	if((cdev_add(&my_cdev, dev, 1)) < 0){
		printk(KERN_INFO"Cannot add the device to the system...\n");
		goto r_class;
	}

	if((dev_class = class_create(THIS_MODULE, "pstree_class")) == NULL){
		printk(KERN_INFO"Cannot create the struct class...\n");
		goto r_class;
	}

	if((device_create(dev_class, NULL, dev, NULL, "pstree_device")) == NULL){
		printk(KERN_INFO"Cannot create the device...\n");
		goto r_device;
	}

	printk(KERN_INFO"pstree Device driver insert.. done properly...\n");
	return 0;

	r_device:
	class_destroy(dev_class);

	r_class:
	unregister_chrdev_region(dev, 1);
	return -1;
}

void __exit pstree_driver_exit(void){
	device_destroy(dev_class, dev);
	class_destroy(dev_class);
	cdev_del(&my_cdev);
	unregister_chrdev_region(dev, 1);
	printk(KERN_INFO" pstree Device driver is removed successfully...\n");
}

module_init(pstree_driver_init);
module_exit(pstree_driver_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ertan and Emre");
MODULE_DESCRIPTION("The character device driver");