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

//IOCTL method macro's
#define IOCTL_MODE_READ _IOW('p', 0, char*)
#define IOCTL_PID_READ _IOW('p', 1, int32_t*)

//User inputs
int32_t pid;
char mode[8];

dev_t dev = 0;

//Queue entry struct for bfs 
struct queue_entry{
	char name[128];
	int id;
	struct list_head lst;
};

//List head of bfs queue
static LIST_HEAD(task_queue);

static struct class *dev_class;
static struct cdev my_cdev;

//Function prototypes
static int __init pstraverse_driver_init(void);
static void __exit pstraverse_driver_exit(void);
static int pstraverse_open(struct inode *inode, struct file *file);
static int pstraverse_release(struct inode *inode, struct file *file);
static ssize_t pstraverse_read(struct file *filp, char __user *buf, size_t len, loff_t* off);
static ssize_t pstraverse_write(struct file *filp, const char __user *buf, size_t len, loff_t* off);
static long pstraverse_ioctl(struct file *file, unsigned int cmd, unsigned long arg);
static void process_command(void);
static void bfs(struct task_struct *task);
static void bfs_initiate(struct task_struct *task);
static void print_queue(void);
static void clean_queue(void);
static void dfs(struct task_struct *task);
static void dfs_initiate(struct task_struct *task);

//Driver mappings
static struct file_operations fops = 
{
	.owner	= THIS_MODULE,
	.read = pstraverse_read,
	.write = pstraverse_write,
	.open = pstraverse_open,
	.release = pstraverse_release,
	.unlocked_ioctl = pstraverse_ioctl,
};

static int pstraverse_open(struct inode *inode, struct file *file){
	printk(KERN_INFO"Module opened successfully. \n");
	return 0;
}


static int pstraverse_release(struct inode *inode, struct file *file){
	printk(KERN_INFO"Module released successfully.\n");
	return 0;
}

//Main IOCTL function that reads pid and mode of search from user space
static long pstraverse_ioctl(struct file *file, unsigned int cmd, unsigned long arg){
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

//This function determines the mode and executes the algorithm accordingly
static void process_command(){
	if(strcmp(mode, "-d") == 0){
		dfs_initiate(&init_task);
	}
	if(strcmp(mode, "-b") == 0){
		bfs_initiate(&init_task);
	}
}
//Initalizer for dfs that searches for the task that has the corresponding pid
static void dfs_initiate(struct task_struct *task){
    for_each_process(task) {
    	if ((int) pid == task->pid) {
	    	dfs(task);
	    	print_queue();
	    	break;
		}
    }
}

//Initalizer for bfs that searches for the task that has the corresponding pid
static void bfs_initiate(struct task_struct *task){
	for_each_process(task){
		if((int) pid == task->pid){
			bfs(task);
			print_queue();
			break;
		}
	}
}
/**
 * This function takes a task as root and traverses one branch as long as it goes.
 * 
 * @param  task root of the process tree thats going to be 
 * 				traversed.
 *
 */
static void dfs(struct task_struct *task) {
    struct task_struct *current_task;
    struct list_head *list;
    struct queue_entry *new;

    new = kmalloc(sizeof(*new), GFP_KERNEL);
    strcpy(new->name, task->comm);
    new->id = task->pid;

    INIT_LIST_HEAD(&new->lst);
    list_add_tail(&new->lst, &task_queue);

    list_for_each(list, &task->children) {
        current_task = list_entry(list, struct task_struct, sibling);
		dfs(current_task);
    }
}

/**
 * This function takes a task as root and traverses its children first
 * and adds it to a queue, then it recursively calls itself to add grandchildren
 * and vice versa.
 * 
 * @param  task root of the process tree thats going to be 
 * 				traversed.
 *
 */
static void bfs(struct task_struct *task){
	struct task_struct *current_task;
	struct list_head *list;
	struct queue_entry *new;

	list_for_each(list, &task->children) {
    	current_task = list_entry(list, struct task_struct, sibling);
    	
    	new = kmalloc(sizeof(*new), GFP_KERNEL);
    	strcpy(new->name, current_task->comm);
    	new->id = current_task->pid;
    	
    	INIT_LIST_HEAD(&new->lst);
    	list_add_tail(&new->lst, &task_queue);
    }

    list_for_each(list, &task->children){
    	current_task = list_entry(list, struct task_struct, sibling);
    	bfs(current_task);
    }
}
/**
 * Prints the bfs queue
 */
static void print_queue(){
	struct queue_entry *current_entry;
	struct list_head *list;

	list_for_each(list, &task_queue){
		current_entry = list_entry(list, struct queue_entry, lst);
		printk(KERN_INFO"PID: %d, Name: %s\n", current_entry->id, current_entry->name);
	}
}

//Frees the memory thats allocated to the bfs queue
static void clean_queue(){
	struct queue_entry *current_entry;
	struct list_head *list;

	list_for_each(list, &task_queue){
		current_entry = list_entry(list, struct queue_entry, lst);
		kfree(current_entry);
	}
}

static ssize_t pstraverse_write(struct file *filp, const char __user *buf, size_t len, loff_t* off){
	return len;
}

static ssize_t pstraverse_read(struct file *filp, char __user *buf, size_t len, loff_t* off){
	return len;
}

static int __init pstraverse_driver_init(void){
	if((alloc_chrdev_region(&dev, 0, 1, "pstraverse_driver")) < 0){
		printk(KERN_INFO"Cannot allocate the major number...\n");
	}

	printk(KERN_INFO"Pstraverse Major = %d, Minor = %d..\n", MAJOR(dev), MINOR(dev));

	cdev_init(&my_cdev, &fops);

	if((cdev_add(&my_cdev, dev, 1)) < 0){
		printk(KERN_INFO"Cannot add the device to the system...\n");
		goto r_class;
	}

	if((dev_class = class_create(THIS_MODULE, "pstraverse_class")) == NULL){
		printk(KERN_INFO"Cannot create the struct class...\n");
		goto r_class;
	}

	if((device_create(dev_class, NULL, dev, NULL, "pstraverse_device")) == NULL){
		printk(KERN_INFO"Cannot create the device...\n");
		goto r_device;
	}

	printk(KERN_INFO"Pstraverse Device driver insert.. done properly...\n");
	return 0;

	r_device:
	class_destroy(dev_class);

	r_class:
	unregister_chrdev_region(dev, 1);
	return -1;
}

void __exit pstraverse_driver_exit(void){
	clean_queue();
	device_destroy(dev_class, dev);
	class_destroy(dev_class);
	cdev_del(&my_cdev);
	unregister_chrdev_region(dev, 1);
	printk(KERN_INFO"Pstraverse Device driver is removed successfully...\n");
}

module_init(pstraverse_driver_init);
module_exit(pstraverse_driver_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ertan and Emre");
MODULE_DESCRIPTION("Pstraverse driver that searches for a specific task and traverses it using depth-first-search or breadth-first-search.");
