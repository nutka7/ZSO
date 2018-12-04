#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/cdev.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/ioctl.h>
#include <linux/anon_inodes.h>
#include <linux/mutex.h>
#include <linux/completion.h>
#include <asm/uaccess.h>
#include <asm/spinlock.h>

#include "harddoom.h"
#include "doomdev.h"
#include "doomcode.h"


MODULE_LICENSE("GPL");
MODULE_AUTHOR("Tom Macieszczak");
MODULE_DESCRIPTION("HardDoom driver");


/* -- MACROS -- */

#define MAX_DEV_NUM 256
#define MAP_SIZE 0x100
#define SPAN_MASK 0x3FFFFF


#define HD_PRINT(Level, Format, ...) \
    printk(Level "HardDoom:%s:%d:" Format "\n", __func__, __LINE__, ##__VA_ARGS__)

#define HD_NOTICE(Format, ...) \
//    HD_PRINT(KERN_NOTICE, Format, ##__VA_ARGS__)

#define return_err(ret)          \
do {                             \
    HD_NOTICE("return %d", ret); \
    return ret;                  \
} while(0)                       \

#define errjmp(Label)    \
do {                     \
    HD_NOTICE("errjmp"); \
    goto Label;          \
} while (0)              \

#define errjmp2(Assignment, Label) \
do {                               \
    Assignment;                    \
    HD_NOTICE("errjmp");           \
    goto Label;                    \
} while (0)                        \

#define copy_object_from_user(object, userbuf) \
    copy_from_user(&(object), (const void __user *) userbuf, sizeof(object))

#define copy_object_from_user_array(object, userbuf, index)     \
    copy_from_user(                                             \
        &(object),                                              \
        (const void __user *) userbuf + index * sizeof(object), \
        sizeof(object))

/** Template for the body of *_release functions */
#define SYNCED_RELEASE(type, resource, release)  \
{                                                \
    struct hd_dev *dev;                          \
    dev = ((type *) (resource))->dev;            \
    if (mutex_lock_interruptible(&dev->mutex))   \
         return_err(-ERESTARTSYS);               \
    hd_sync(dev);                                \
    release(resource);                           \
    mutex_unlock(&dev->mutex);                   \
    kref_put(&dev->refcount, hd_release);        \
    return 0;                                    \
}

/* -- TYPES -- */

struct hd_dev {
    void __iomem *bar0;
    struct dma_pool *page_pool;
    struct dma_pool *map_pool;
    struct cdev cdev;
    struct device *device;
    u64 interlock;
    struct mutex mutex;
    struct completion sync_compl;
    struct completion async_compl;
    struct completion remove_compl;
    struct kref refcount;
    u16 free_cmds;
    u16 ping_async;
};

struct dma_block {
    void *virt;
    dma_addr_t dma;
};

struct paged_buf {
    size_t page_num;
    struct dma_block *addr;
    dma_addr_t page_table;
};

struct surface {
    struct hd_dev *dev;
    u16 width;
    u16 height;
    struct paged_buf pbuf;
    u64 interlock;
};

struct texture {
    struct hd_dev *dev;
    u32 size;
    u16 height;
    struct paged_buf pbuf;
};

struct flat {
    struct hd_dev *dev;
    void *virt;
    dma_addr_t dma;
};

struct colormaps {
    struct hd_dev *dev;
    struct dma_block *addr;
    u32 num;
};


static dev_t hd_major;

static struct file_operations surface_fops;
static struct file_operations texture_fops;
static struct file_operations flat_fops;
static struct file_operations colormaps_fops;

static DEFINE_SPINLOCK(minor_lock);
static u8 minor_in_use[MAX_DEV_NUM];

static int getminor(void)
{
    int i;

    spin_lock(&minor_lock);
    for (i = 0; i < MAX_DEV_NUM; ++i)
        if (!minor_in_use[i]) {
            minor_in_use[i] = 1;
            break;
        }
    spin_unlock(&minor_lock);

    if (i < MAX_DEV_NUM)
        return i;
    else
        return -ENOMEM;
}

static void putminor(int minor)
{
    spin_lock(&minor_lock);
    minor_in_use[minor] = 0;
    spin_unlock(&minor_lock);
}

/* -- GETTERS -- */

static struct surface *get_surface(struct hd_dev *dev, u32 fd,
                                   struct surface *other)
{
    struct file *file;
    struct surface *surf;

    file = fget(fd);
    if (!file || file->f_op != &surface_fops)
        errjmp(err_invalid);

    surf = file->private_data;
    if (surf->dev != dev)
        errjmp(err_invalid);

    if (surf->width != other->width || surf->height != other->height)
        errjmp(err_invalid);

    fput(file);
    return surf;

err_invalid:
    if (file) fput(file);
    return NULL;
}

static struct texture *get_texture(struct hd_dev *dev, u32 fd)
{
    struct file *file;
    struct texture *text;

    file = fget(fd);
    if (!file || file->f_op != &texture_fops)
        errjmp(err_invalid);

    text = file->private_data;
    if (text->dev != dev)
        errjmp(err_invalid);

    fput(file);
    return text;

err_invalid:
    if (file) fput(file);
    return NULL;
}

static struct flat *get_flat(struct hd_dev *dev, u32 fd)
{
    struct file *file;
    struct flat *flat;

    file = fget(fd);
    if (!file || file->f_op != &flat_fops)
        errjmp(err_invalid);

    flat = file->private_data;
    if (flat->dev != dev)
        errjmp(err_invalid);

    fput(file);
    return flat;

err_invalid:
    if (file) fput(file);
    return NULL;
}

static struct colormaps *get_colormaps(struct hd_dev *dev, u32 fd)
{
    struct file *file;
    struct colormaps *cmap;

    file = fget(fd);
    if (!file || file->f_op != &colormaps_fops)
        errjmp(err_invalid);

    cmap = file->private_data;
    if (cmap->dev != dev)
        errjmp(err_invalid);

    fput(file);
    return cmap;

err_invalid:
    if (file) fput(file);
    return NULL;
}

static inline void hd_iowrite(struct hd_dev *dev, u64 reg, u32 val)
{
    iowrite32(val, dev->bar0 + reg);
}

static inline u32 hd_ioread(struct hd_dev *dev, u64 reg)
{
    return ioread32(dev->bar0 + reg);
}

static void hd_load_microcode(struct hd_dev *dev)
{
    size_t i;

    hd_iowrite(dev, HARDDOOM_FE_CODE_ADDR, 0);

    for (i = 0; i < ARRAY_SIZE(doomcode); ++i)
        hd_iowrite(dev, HARDDOOM_FE_CODE_WINDOW, doomcode[i]);
}

static void hd_turn_on(struct hd_dev *dev)
{
    hd_load_microcode(dev);
    hd_iowrite(dev, HARDDOOM_RESET, HARDDOOM_RESET_ALL);
    hd_iowrite(dev, HARDDOOM_INTR,  HARDDOOM_INTR_MASK);
    hd_iowrite(dev, HARDDOOM_INTR_ENABLE, HARDDOOM_INTR_PONG_SYNC);
    hd_iowrite(dev, HARDDOOM_ENABLE,
                    HARDDOOM_ENABLE_ALL ^ HARDDOOM_ENABLE_FETCH_CMD);
}

static void hd_turn_off(struct hd_dev *dev)
{
    hd_iowrite(dev, HARDDOOM_ENABLE, 0);
    hd_iowrite(dev, HARDDOOM_INTR_ENABLE, 0);
    hd_ioread(dev, HARDDOOM_ENABLE);
}

static void _hd_cmd(struct hd_dev *dev, u32 cmd)
{
    if (!dev->free_cmds)
        dev->free_cmds = hd_ioread(dev, HARDDOOM_FIFO_FREE);

    if (!dev->free_cmds)
    {
        hd_iowrite(dev, HARDDOOM_INTR, HARDDOOM_INTR_PONG_ASYNC);
        dev->free_cmds = hd_ioread(dev, HARDDOOM_FIFO_FREE);

        if (!dev->free_cmds) {
            reinit_completion(&dev->async_compl);
            hd_iowrite(dev, HARDDOOM_INTR_ENABLE,
                       HARDDOOM_INTR_PONG_SYNC | HARDDOOM_INTR_PONG_ASYNC);
            wait_for_completion(&dev->async_compl);

            dev->free_cmds = hd_ioread(dev, HARDDOOM_FIFO_FREE);
        }
    }

    --dev->free_cmds;

    hd_iowrite(dev, HARDDOOM_FIFO_SEND, cmd);
}

static void hd_cmd(struct hd_dev *dev, u32 cmd)
{
    dev->ping_async = (dev->ping_async + 1) % (512 / 4);

    if (!dev->ping_async)
        _hd_cmd(dev, HARDDOOM_CMD_PING_ASYNC);

    _hd_cmd(dev, cmd);
}

static void hd_sync(struct hd_dev *dev)
{
    reinit_completion(&dev->sync_compl);
    hd_cmd(dev, HARDDOOM_CMD_PING_SYNC);
    wait_for_completion(&dev->sync_compl);
}

static irqreturn_t hd_irq_handler(int irq, void *dev)
{
    u32 intr;

    intr = hd_ioread(dev, HARDDOOM_INTR);
    if (!intr) return IRQ_NONE;

    hd_iowrite(dev, HARDDOOM_INTR,  intr);

    if (intr & HARDDOOM_INTR_PONG_ASYNC) {
        hd_iowrite(dev, HARDDOOM_INTR_ENABLE, HARDDOOM_INTR_PONG_SYNC);
        complete(&((struct hd_dev *) dev)->async_compl);
    }

    if (intr & HARDDOOM_INTR_PONG_SYNC)
        complete(&((struct hd_dev *) dev)->sync_compl);

    if (intr & ~(HARDDOOM_INTR_PONG_ASYNC | HARDDOOM_INTR_PONG_SYNC))
        HD_PRINT(KERN_ERR, "unexpected interrupt mask: %x %x %x",
            intr,
            hd_ioread(dev, HARDDOOM_FE_ERROR_CODE),
            hd_ioread(dev, 0x2c));

    return IRQ_HANDLED;
}

static void hd_release(struct kref *kref)
{
    struct hd_dev *dev;

    dev = container_of(kref, struct hd_dev, refcount);
    complete(&dev->remove_compl);
}


static void free_dma_blocks(struct dma_pool *pool,
                            struct dma_block *blocks, size_t n)
{
    while (n--)
        dma_pool_free(pool, blocks[n].virt, blocks[n].dma);
    kfree(blocks);
}

static struct dma_block *zalloc_dma_blocks(struct dma_pool *pool, size_t n)
{
    int i;
    struct dma_block *blocks;

    blocks = kmalloc_array(n, sizeof(*blocks), GFP_KERNEL);
    if (!blocks) errjmp(err_kmalloc);

    for (i = 0; i < n; ++i) {
        blocks[i].virt = dma_pool_zalloc(pool, GFP_KERNEL, &blocks[i].dma);
        if (!blocks[i].virt) errjmp(err_pool);
    }

    return blocks;

err_pool:
    free_dma_blocks(pool, blocks, i);
err_kmalloc:
    return NULL;
}

static int alloc_paged_buffer(struct hd_dev *dev,
                              struct paged_buf *pbuf, size_t len)
{
    size_t page_num;
    size_t pt_offset;
    size_t i;
    u32 *page_table;

    /* The page table is always located on the last page.
     * We allocate an additional page for it if needed.
     */

    len = roundup(len, 64); // page table alignment
    page_num = DIV_ROUND_UP(len, PAGE_SIZE);

    if (page_num * PAGE_SIZE - len >= page_num * sizeof(u32)) {
        pt_offset = len % PAGE_SIZE;
    } else {
        page_num += 1;
        pt_offset = 0;
    }
    pbuf->page_num = page_num;

    pbuf->addr = zalloc_dma_blocks(dev->page_pool, page_num);
    if (!pbuf->addr) return_err(-ENOMEM);

    page_table = pbuf->addr[page_num - 1].virt + pt_offset;

    for (i = 0; i < page_num; ++i)
        page_table[i] =  pbuf->addr[i].dma | HARDDOOM_PTE_VALID;

    pbuf->page_table = pbuf->addr[page_num - 1].dma + pt_offset;

    return 0;
}

static void free_paged_buffer(struct hd_dev *dev, struct paged_buf *pbuf)
{
    free_dma_blocks(dev->page_pool, pbuf->addr, pbuf->page_num);
}

static int bad_point(struct surface *surf, u16 x, u16 y)
{
    return !(x < surf->width && y < surf->height);
}

static int bad_rect(struct surface *surf, u16 x, u16 y, u16 w, u16 h)
{
    return (u32) x + (u32) w > (u32) surf->width ||
           (u32) y + (u32) h > (u32) surf->height;
}

static int bad_fixpoint(u32 fixpoint_num) {
    return fixpoint_num & 0x3f << 26;
}


long surf_fill_rects(struct surface *surf,
                     struct doomdev_surf_ioctl_fill_rects cmd)
{
    int err = 0;
    long count = 0;
    struct hd_dev *dev;
    struct doomdev_fill_rect subcmd;

    dev = surf->dev;

    if (mutex_lock_interruptible(&dev->mutex))
        return_err(-ERESTARTSYS);

    hd_cmd(dev, HARDDOOM_CMD_SURF_DST_PT(surf->pbuf.page_table));
    hd_cmd(dev, HARDDOOM_CMD_SURF_DIMS(surf->width, surf->height));

    while (count < cmd.rects_num) {
        if (copy_object_from_user_array(subcmd, cmd.rects_ptr, count)) {
            err = -EFAULT;
            break;
        }

        /* ctest.c allows for empty rects with bad coordinates */
        if (!subcmd.width || !subcmd.height) {
            count++;
            continue;
        }

        if (bad_rect(surf,
                     subcmd.pos_dst_x, subcmd.pos_dst_y,
                     subcmd.width, subcmd.height))
        {
            err = -EINVAL;
            break;
        }

        hd_cmd(dev, HARDDOOM_CMD_XY_A(subcmd.pos_dst_x, subcmd.pos_dst_y));
        hd_cmd(dev, HARDDOOM_CMD_FILL_COLOR(subcmd.color));
        hd_cmd(dev, HARDDOOM_CMD_FILL_RECT(subcmd.width, subcmd.height));

        count++;
    }
    surf->interlock = dev->interlock;
    mutex_unlock(&dev->mutex);

    if (!count && err) return_err(err);
    return count;
}

long surf_copy_rects(struct surface *surf,
                     struct doomdev_surf_ioctl_copy_rects cmd)
{
    int err = 0;
    long count = 0;
    struct hd_dev *dev;
    struct doomdev_copy_rect subcmd;
    struct surface *src_surf;

    dev = surf->dev;

    if (mutex_lock_interruptible(&dev->mutex))
        return_err(-ERESTARTSYS);

    src_surf = get_surface(dev, cmd.surf_src_fd, surf);
    if (!src_surf)
        errjmp2(err = -EINVAL, err_invalid);

    if (src_surf->interlock == dev->interlock) {
        hd_cmd(dev, HARDDOOM_CMD_INTERLOCK);
        dev->interlock++;
    }

    hd_cmd(dev, HARDDOOM_CMD_SURF_DST_PT(surf->pbuf.page_table));
    hd_cmd(dev, HARDDOOM_CMD_SURF_SRC_PT(src_surf->pbuf.page_table));
    hd_cmd(dev, HARDDOOM_CMD_SURF_DIMS(surf->width, surf->height));

    while (count < cmd.rects_num) {
        if (copy_object_from_user_array(subcmd, cmd.rects_ptr, count)) {
            err = -EFAULT;
            break;
        }

        /* ctest.c allows for empty rects with bad coordinates */
        if (!subcmd.width || !subcmd.height) {
            count++;
            continue;
        }

        if (bad_rect(surf,
                     subcmd.pos_dst_x, subcmd.pos_dst_y,
                     subcmd.width, subcmd.height) ||
            bad_rect(src_surf,
                     subcmd.pos_src_x, subcmd.pos_src_y,
                     subcmd.width, subcmd.height))
        {
            err = -EINVAL;
            break;
        }

        hd_cmd(dev, HARDDOOM_CMD_XY_A(subcmd.pos_dst_x, subcmd.pos_dst_y));
        hd_cmd(dev, HARDDOOM_CMD_XY_B(subcmd.pos_src_x, subcmd.pos_src_y));
        hd_cmd(dev, HARDDOOM_CMD_COPY_RECT(subcmd.width, subcmd.height));

        count++;
    }
    surf->interlock = dev->interlock;

err_invalid:
    mutex_unlock(&dev->mutex);

    if (!count && err) return_err(err);
    return count;
}

long surf_draw_lines(struct surface *surf,
                     struct doomdev_surf_ioctl_draw_lines cmd)
{
    int err = 0;
    long count = 0;
    struct hd_dev *dev;
    struct doomdev_line subcmd;

    dev = surf->dev;

    if (mutex_lock_interruptible(&dev->mutex))
        return_err(-ERESTARTSYS);

    hd_cmd(dev, HARDDOOM_CMD_SURF_DST_PT(surf->pbuf.page_table));
    hd_cmd(dev, HARDDOOM_CMD_SURF_DIMS(surf->width, surf->height));

    while (count < cmd.lines_num) {
        if (copy_object_from_user_array(subcmd, cmd.lines_ptr, count)) {
            err = -EFAULT;
            break;
        }

        if (bad_point(surf, subcmd.pos_a_x, subcmd.pos_a_y) ||
            bad_point(surf, subcmd.pos_b_x, subcmd.pos_b_y))
        {
            err = -EINVAL;
            break;
        }

        hd_cmd(dev, HARDDOOM_CMD_XY_A(subcmd.pos_a_x, subcmd.pos_a_y));
        hd_cmd(dev, HARDDOOM_CMD_XY_B(subcmd.pos_b_x, subcmd.pos_b_y));
        hd_cmd(dev, HARDDOOM_CMD_FILL_COLOR(subcmd.color));
        hd_cmd(dev, HARDDOOM_CMD_DRAW_LINE);

        count++;
    }
    surf->interlock = dev->interlock;
    mutex_unlock(&dev->mutex);

    if (!count && err) return_err(err);
    return count;
}

long surf_draw_background(struct surface *surf,
                          struct doomdev_surf_ioctl_draw_background cmd)
{
    int err = 0;
    struct hd_dev *dev;
    struct flat *flat;

    dev = surf->dev;

    if (mutex_lock_interruptible(&dev->mutex))
        return_err(-ERESTARTSYS);

    flat = get_flat(dev, cmd.flat_fd);
    if (!flat)
        errjmp2(err = -EINVAL, err_invalid);

    hd_cmd(dev, HARDDOOM_CMD_SURF_DST_PT(surf->pbuf.page_table));
    hd_cmd(dev, HARDDOOM_CMD_SURF_DIMS(surf->width, surf->height));
    hd_cmd(dev, HARDDOOM_CMD_FLAT_ADDR(flat->dma));
    hd_cmd(dev, HARDDOOM_CMD_DRAW_BACKGROUND);

    surf->interlock = dev->interlock;

err_invalid:
    mutex_unlock(&dev->mutex);

    return err;
}

long surf_draw_columns(struct surface *surf,
                       struct doomdev_surf_ioctl_draw_columns cmd)
{
    int err = 0;
    long count = 0;
    struct hd_dev *dev;
    struct doomdev_column subcmd;
    u8 fuzz;
    u8 translate;
    u8 colormap;
    u8 flags;
    struct texture *text = NULL;   // suppress warning
    struct colormaps *tran = NULL; // suppress warning
    struct colormaps *cmap = NULL; // suppress warning
    u8 prev_idx;

    dev = surf->dev;

    if (mutex_lock_interruptible(&dev->mutex))
        return_err(-ERESTARTSYS);

    fuzz = cmd.draw_flags & HARDDOOM_DRAW_PARAMS_FUZZ;
    translate = ~fuzz & cmd.draw_flags & HARDDOOM_DRAW_PARAMS_TRANSLATE;
    colormap = ~fuzz & cmd.draw_flags & HARDDOOM_DRAW_PARAMS_COLORMAP;

    flags = fuzz | translate | colormap;

    if (!fuzz) {
        text = get_texture(dev, cmd.texture_fd);
        if (!text)
            errjmp2(err = -EINVAL, err_invalid);
    }
    if (translate) {
        tran = get_colormaps(dev, cmd.translations_fd);
        if (!tran || cmd.translation_idx >= tran->num)
            errjmp2(err = -EINVAL, err_invalid);
    }
    if (colormap || fuzz) {
        cmap = get_colormaps(dev, cmd.colormaps_fd);
        if (!cmap)
            errjmp2(err = -EINVAL, err_invalid);
    }

    hd_cmd(dev, HARDDOOM_CMD_SURF_DST_PT(surf->pbuf.page_table));
    hd_cmd(dev, HARDDOOM_CMD_SURF_DIMS(surf->width, surf->height));
    hd_cmd(dev, HARDDOOM_CMD_DRAW_PARAMS(flags));

    if (!fuzz) {
        hd_cmd(dev, HARDDOOM_CMD_TEXTURE_PT(text->pbuf.page_table));
        hd_cmd(dev, HARDDOOM_CMD_TEXTURE_DIMS(text->size, text->height));
    }
    if (translate) {
        dma_addr_t addr;

        addr = tran->addr[cmd.translation_idx].dma;
        hd_cmd(dev, HARDDOOM_CMD_TRANSLATION_ADDR(addr));
    }

    while (count < cmd.columns_num) {
        if (copy_object_from_user_array(subcmd, cmd.columns_ptr, count)) {
            err = -EFAULT;
            break;
        }

        if (subcmd.y1 > subcmd.y2)
            swap(subcmd.y1, subcmd.y2);

        if ((!fuzz && (
                bad_fixpoint(subcmd.ustart) ||
                bad_fixpoint(subcmd.ustep)))
            || ((fuzz || colormap) && subcmd.colormap_idx >= cmap->num))
        {
            err = -EFAULT;
            break;
        }

        hd_cmd(dev, HARDDOOM_CMD_XY_A(subcmd.x, subcmd.y1));
        hd_cmd(dev, HARDDOOM_CMD_XY_B(subcmd.x, subcmd.y2));

        if (!fuzz) {
            hd_cmd(dev, HARDDOOM_CMD_USTART(subcmd.ustart));
            hd_cmd(dev, HARDDOOM_CMD_USTEP(subcmd.ustep));
        }
        if ((fuzz || colormap) && (!count || prev_idx != subcmd.colormap_idx))        {
            dma_addr_t addr;

            prev_idx = subcmd.colormap_idx;
            addr = cmap->addr[subcmd.colormap_idx].dma;
            hd_cmd(dev, HARDDOOM_CMD_COLORMAP_ADDR(addr));
        }
        hd_cmd(dev, HARDDOOM_CMD_DRAW_COLUMN(subcmd.texture_offset));

        count++;
    }
    surf->interlock = dev->interlock;

err_invalid:
    mutex_unlock(&dev->mutex);

    if (!count && err) return_err(err);
    return count;
}

long surf_draw_spans(struct surface *surf,
                     struct doomdev_surf_ioctl_draw_spans cmd)
{
    int err = 0;
    long count = 0;
    struct hd_dev *dev;
    struct doomdev_span subcmd;
    u8 translate;
    u8 colormap;
    u8 flags;
    struct flat *flat;
    struct colormaps *tran = NULL; // suppress warning
    struct colormaps *cmap = NULL; // suppress warning
    u8 prev_idx = 0;               // suppress warning

    dev = surf->dev;

    if (mutex_lock_interruptible(&dev->mutex))
        return_err(-ERESTARTSYS);

    translate = cmd.draw_flags & HARDDOOM_DRAW_PARAMS_TRANSLATE;
    colormap = cmd.draw_flags & HARDDOOM_DRAW_PARAMS_COLORMAP;
    flags = translate | colormap;

    flat = get_flat(dev, cmd.flat_fd);
    if (!flat)
        errjmp2(err = -EINVAL, err_invalid);

    if (translate) {
        tran = get_colormaps(dev, cmd.translations_fd);
        if (!tran || cmd.translation_idx >= tran->num)
            errjmp2(err = -EINVAL, err_invalid);
    }

    if (colormap) {
        cmap = get_colormaps(dev, cmd.colormaps_fd);
        if (!cmap)
            errjmp2(err = -EINVAL, err_invalid);
    }

    hd_cmd(dev, HARDDOOM_CMD_SURF_DST_PT(surf->pbuf.page_table));
    hd_cmd(dev, HARDDOOM_CMD_SURF_DIMS(surf->width, surf->height));
    hd_cmd(dev, HARDDOOM_CMD_FLAT_ADDR(flat->dma));
    hd_cmd(dev, HARDDOOM_CMD_DRAW_PARAMS(flags));
    if (translate) {
        dma_addr_t addr;

        addr = tran->addr[cmd.translation_idx].dma;
        hd_cmd(dev, HARDDOOM_CMD_TRANSLATION_ADDR(addr));
    }

    while (count < cmd.spans_num) {
        if (copy_object_from_user_array(subcmd, cmd.spans_ptr, count)) {
            err = -EFAULT;
            break;
        }

        if (subcmd.x1 > subcmd.x2)
            swap(subcmd.x1, subcmd.x2);

        if (colormap && subcmd.colormap_idx >= cmap->num) {
            err = -EFAULT;
            break;
        }

        hd_cmd(dev, HARDDOOM_CMD_USTART(subcmd.ustart & SPAN_MASK));
        hd_cmd(dev, HARDDOOM_CMD_VSTART(subcmd.vstart & SPAN_MASK));
        hd_cmd(dev, HARDDOOM_CMD_USTEP(subcmd.ustep & SPAN_MASK));
        hd_cmd(dev, HARDDOOM_CMD_VSTEP(subcmd.vstep & SPAN_MASK));
        hd_cmd(dev, HARDDOOM_CMD_XY_A(subcmd.x1, subcmd.y));
        hd_cmd(dev, HARDDOOM_CMD_XY_B(subcmd.x2, subcmd.y));
        if (colormap && (!count || prev_idx != subcmd.colormap_idx)) {
            dma_addr_t addr;

            prev_idx = subcmd.colormap_idx;
            addr = cmap->addr[subcmd.colormap_idx].dma;
            hd_cmd(dev, HARDDOOM_CMD_COLORMAP_ADDR(addr));
        }
        hd_cmd(dev, HARDDOOM_CMD_DRAW_SPAN);

        count++;
    }
    surf->interlock = dev->interlock;

err_invalid:
    mutex_unlock(&dev->mutex);

    if (!count && err) return_err(err);
    return count;
}

long surface_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct surface *surf;
    union {
        struct doomdev_surf_ioctl_fill_rects      fill_rects;
        struct doomdev_surf_ioctl_draw_lines      draw_lines;
        struct doomdev_surf_ioctl_draw_background draw_background;
        struct doomdev_surf_ioctl_draw_columns    draw_columns;
        struct doomdev_surf_ioctl_copy_rects      copy_rects;
        struct doomdev_surf_ioctl_draw_spans      draw_spans;
    } surf_cmd;

    if (_IOC_SIZE(cmd) > sizeof(surf_cmd))
        return_err(-EINVAL);

    if (copy_from_user(&surf_cmd, (void __user *) arg, _IOC_SIZE(cmd)))
        return_err(-EFAULT);

    surf = file->private_data;

    switch (cmd) {
    case DOOMDEV_SURF_IOCTL_COPY_RECTS:
        return surf_copy_rects(surf, surf_cmd.copy_rects);
    case DOOMDEV_SURF_IOCTL_FILL_RECTS:
        return surf_fill_rects(surf, surf_cmd.fill_rects);
    case DOOMDEV_SURF_IOCTL_DRAW_LINES:
        return surf_draw_lines(surf, surf_cmd.draw_lines);
    case DOOMDEV_SURF_IOCTL_DRAW_BACKGROUND:
        return surf_draw_background(surf, surf_cmd.draw_background);
    case DOOMDEV_SURF_IOCTL_DRAW_COLUMNS:
        return surf_draw_columns(surf, surf_cmd.draw_columns);
    case DOOMDEV_SURF_IOCTL_DRAW_SPANS:
        return surf_draw_spans(surf, surf_cmd.draw_spans);
    }
    return -EINVAL;
}

static ssize_t surface_read(struct file *file, char __user *buf,
                            size_t count, loff_t *filepos)
{
    int err = 0;
    struct surface *surf;
    size_t len;
    loff_t pos;
    size_t left;

    surf = file->private_data;

    if (mutex_lock_interruptible(&surf->dev->mutex))
        return_err(-ERESTARTSYS);

    hd_sync(surf->dev);

    len = (size_t) surf->width * (size_t) surf->height;
    pos = *filepos;

    if (pos >= len || pos < 0) return 0;

    if (count > len - pos)
        count = len - pos;

    left = count;
    while (left) {
        size_t i;
        size_t n;
        void *from;
        char __user *to;

        i = pos / PAGE_SIZE;
        n = min((size_t) (PAGE_SIZE - pos % PAGE_SIZE), left);
        from = surf->pbuf.addr[i].virt + pos % PAGE_SIZE;
        to = buf + count - left;

        if (copy_to_user(to, from, n))
            errjmp2(err = -EFAULT, err_copy);

        pos += n;
        left -= n;
    }

    *filepos = pos;

err_copy:
    mutex_unlock(&surf->dev->mutex);
    return err ? err : count;
}

static void free_surface(struct surface *surf)
{
    free_paged_buffer(surf->dev, &surf->pbuf);
    kfree(surf);
}

static int surface_release(struct inode *inode, struct file *file)
    SYNCED_RELEASE(struct surface, file->private_data, free_surface)

static struct file_operations surface_fops = {
    .owner = THIS_MODULE,
    .read = surface_read,
    .unlocked_ioctl = surface_ioctl,
    .compat_ioctl = surface_ioctl,
    .release = surface_release,
};

static long create_surface(struct hd_dev *dev,
                           struct doomdev_ioctl_create_surface cmd)
{
    int err;
    struct surface *surf;
    size_t len;
    int fd;
    struct file *file;

    if (cmd.width % 64 || !cmd.width || !cmd.height)
        return_err(-EINVAL);
    if (cmd.width > 2048 || cmd.height > 2048)
        return_err(-EOVERFLOW);

    surf = kmalloc(sizeof(*surf), GFP_KERNEL);
    if (!surf) errjmp2(err = -ENOMEM, err_kmalloc);

    surf->dev = dev;
    surf->width = cmd.width;
    surf->height = cmd.height;
    surf->interlock = 0;

    len = (size_t) cmd.width * (size_t) cmd.height;

    err = alloc_paged_buffer(dev, &surf->pbuf, len);
    if (err) errjmp(err_buffer);

    fd = get_unused_fd_flags(0);
    if (fd < 0) errjmp2(err = fd, err_fd);

    file = anon_inode_getfile("HardDoomSurface", &surface_fops, surf, 0);
    if (IS_ERR(file)) errjmp2(err = PTR_ERR(file), err_getfile);

    file->f_mode |= FMODE_LSEEK | FMODE_PREAD | FMODE_PWRITE;

    kref_get(&dev->refcount);

    fd_install(fd, file);

    return fd;

err_getfile:
    put_unused_fd(fd);
err_fd:
    free_paged_buffer(dev, &surf->pbuf);
err_buffer:
    kfree(surf);
err_kmalloc:
    return err;
}

static void free_texture(struct texture *text)
{
    free_paged_buffer(text->dev, &text->pbuf);
    kfree(text);
}

static int texture_release(struct inode *inode, struct file *file)
    SYNCED_RELEASE(struct texture, file->private_data, free_texture)

static struct file_operations texture_fops = {
    .owner = THIS_MODULE,
    .release = texture_release,
};

static long create_texture(struct hd_dev *dev,
                           struct doomdev_ioctl_create_texture cmd)
{
    int err;
    struct texture *text;
    size_t pos;
    size_t left;
    int fd;

    if (!cmd.size)
        return_err(-EINVAL);
    if (cmd.size > (1 << 22) || cmd.height > 1023)
        return_err(-EOVERFLOW);

    text = kmalloc(sizeof(*text), GFP_KERNEL);
    if (!text) errjmp2(err = -ENOMEM, err_kmalloc);

    text->dev = dev;
    text->size = roundup(cmd.size, 256);
    text->height = cmd.height;

    err = alloc_paged_buffer(dev, &text->pbuf, text->size);
    if (err) errjmp(err_buffer);

    pos = 0;
    left = cmd.size;
    while (left) {
        size_t i;
        size_t n;
        void *to;
        const char __user *from;

        i = pos / PAGE_SIZE;
        n = min(PAGE_SIZE, left);
        to = text->pbuf.addr[i].virt;
        from = (void *) cmd.data_ptr + cmd.size - left;

        if (copy_from_user(to, from, n))
            errjmp2(err = -EFAULT, err_getfd);

        pos += n;
        left -= n;
    }

    fd = anon_inode_getfd("HardDoomTexture", &texture_fops, text, 0);
    if (fd < 0) errjmp2(err = fd, err_getfd);

    kref_get(&dev->refcount);

    return fd;

err_getfd:
    free_paged_buffer(dev, &text->pbuf);
err_buffer:
    kfree(text);
err_kmalloc:
    return err;
}

static void free_flat(struct flat *flat)
{
    dma_pool_free(flat->dev->page_pool, flat->virt, flat->dma);
    kfree(flat);
}

static int flat_release(struct inode *inode, struct file *file)
    SYNCED_RELEASE(struct flat, file->private_data, free_flat)

static struct file_operations flat_fops = {
    .owner = THIS_MODULE,
    .release = flat_release,
};

static long create_flat(struct hd_dev *dev,
                        struct doomdev_ioctl_create_flat cmd)
{
    int err;
    struct flat *flat;
    int fd;

    flat = kmalloc(sizeof(*flat), GFP_KERNEL);
    if (!flat) errjmp2(err = -ENOMEM, err_kmalloc);

    flat->dev = dev;
    flat->virt =
        dma_pool_alloc(dev->page_pool, GFP_KERNEL, &flat->dma);
    if (!flat->virt) errjmp2(err = -ENOMEM, err_pool);

    if (copy_from_user(flat->virt, (void *) cmd.data_ptr, PAGE_SIZE))
            errjmp2(err = -EFAULT, err_getfd);

    fd = anon_inode_getfd("HardDoomFlat", &flat_fops, flat, 0);
    if (fd < 0) errjmp2(err = fd, err_getfd);

    kref_get(&dev->refcount);

    return fd;

err_getfd:
    dma_pool_free(dev->page_pool, flat->virt, flat->dma);
err_pool:
    kfree(flat);
err_kmalloc:
    return err;
}

static void free_colormaps(struct colormaps *cmaps)
{
    free_dma_blocks(cmaps->dev->map_pool, cmaps->addr, cmaps->num);
    kfree(cmaps);
}

static int colormaps_release(struct inode *inode, struct file *file)
    SYNCED_RELEASE(struct colormaps, file->private_data, free_colormaps)

static struct file_operations colormaps_fops = {
    .owner = THIS_MODULE,
    .release = colormaps_release,
};

static long create_colormaps(struct hd_dev *dev,
                             struct doomdev_ioctl_create_colormaps cmd)
{
    int err;
    struct colormaps *cmaps;
    size_t i;
    int fd;

    if (!cmd.num)
        return_err(-EINVAL);

    if (cmd.num > 0x100)
        return_err(-EOVERFLOW);

    cmaps = kmalloc(sizeof(*cmaps), GFP_KERNEL);
    if (!cmaps) errjmp2(err = -ENOMEM, err_kmalloc);

    cmaps->dev = dev;
    cmaps->num = cmd.num;
    cmaps->addr = zalloc_dma_blocks(dev->map_pool, cmd.num);
    if (!cmaps->addr) errjmp2(err = -ENOMEM, err_zalloc);

    for (i = 0; i < cmaps->num; ++i)
        if (copy_from_user(cmaps->addr[i].virt,
                           (void *) cmd.data_ptr + i * MAP_SIZE,
                           MAP_SIZE))
            errjmp2(err = -EFAULT, err_getfd);

    fd = anon_inode_getfd("HardDoomColormaps", &colormaps_fops, cmaps, 0);
    if (fd < 0) errjmp2(err = fd, err_getfd);

    kref_get(&dev->refcount);

    return fd;

err_getfd:
    free_dma_blocks(dev->map_pool, cmaps->addr, cmaps->num);
err_zalloc:
    kfree(cmaps);
err_kmalloc:
    return err;
}

static long doom_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct hd_dev *dev;
    union {
        struct doomdev_ioctl_create_surface   surface;
        struct doomdev_ioctl_create_texture   texture;
        struct doomdev_ioctl_create_flat      flat;
        struct doomdev_ioctl_create_colormaps colormaps;
    } doom_cmd;

    if (_IOC_SIZE(cmd) > sizeof(doom_cmd))
        return_err(-EINVAL);

    if (copy_from_user(&doom_cmd, (void __user *) arg, _IOC_SIZE(cmd)))
        return_err(-EFAULT);

    dev = file->private_data;

    switch (cmd) {
    case DOOMDEV_IOCTL_CREATE_SURFACE:
        return create_surface(dev, doom_cmd.surface);
    case DOOMDEV_IOCTL_CREATE_TEXTURE:
        return create_texture(dev, doom_cmd.texture);
    case DOOMDEV_IOCTL_CREATE_FLAT:
        return create_flat(dev, doom_cmd.flat);
    case DOOMDEV_IOCTL_CREATE_COLORMAPS:
        return create_colormaps(dev, doom_cmd.colormaps);
    }
    return -EINVAL;
}

static int doom_open(struct inode *inode, struct file *file)
{
    struct hd_dev *dev;

    dev = container_of(inode->i_cdev, struct hd_dev, cdev);
    kref_get(&dev->refcount);
    file->private_data = container_of(inode->i_cdev, struct hd_dev, cdev);
    return 0;
}

static int doom_release(struct inode *inode, struct file *file)
{
    struct hd_dev *dev;

    dev = file->private_data;
    kref_put(&dev->refcount, hd_release);
    return 0;
}

static struct file_operations doom_fops = {
    .owner = THIS_MODULE,
    .open = doom_open,
    .unlocked_ioctl = doom_ioctl,
    .compat_ioctl = doom_ioctl,
    .release = doom_release,
};

struct class hd_class = {
    .name = "HardDoom",
    .owner = THIS_MODULE,
};

static int hd_init_pci_dev(struct pci_dev *p) {
    int err;
    struct hd_dev *h;

    h = kmalloc(sizeof(*h), GFP_KERNEL);
    if (!h) errjmp2(err = -ENOMEM, err_kmalloc);

    err = pci_enable_device(p);
    if (err) errjmp(err_enable);

    err = pci_request_regions(p, "HardDoom");
    if (err) errjmp(err_request);

    h->bar0 = pci_iomap(p, 0, HARDDOOM_PAGE_SIZE);
    if (!h->bar0) errjmp2(err = -EIO, err_iomap);

    pci_set_master(p);

    err = pci_set_dma_mask(p, DMA_BIT_MASK(32));
    if (err) errjmp(err_mask);

    pci_set_consistent_dma_mask(p, DMA_BIT_MASK(32));
    if (err) errjmp(err_mask);

    h->page_pool =
        dma_pool_create("HardDoom", &p->dev, PAGE_SIZE, PAGE_SIZE, 0);
    if (!h->page_pool) errjmp2(err = -ENOMEM, err_page_pool);

    h->map_pool =
        dma_pool_create("HardDoom", &p->dev, MAP_SIZE, MAP_SIZE, 0);
    if (!h->map_pool) errjmp2(err = -ENOMEM, err_map_pool);

    err = request_irq(p->irq, hd_irq_handler, IRQF_SHARED, "HardDoom", h);
    if (err) errjmp(err_irq);

    hd_turn_on(h);

    h->free_cmds = 0;
    h->ping_async = 0;
    h->interlock = 1;
    mutex_init(&h->mutex);
    init_completion(&h->sync_compl);
    init_completion(&h->async_compl);
    init_completion(&h->remove_compl);
    kref_init(&h->refcount);

    pci_set_drvdata(p, h);
    return 0;

err_irq:
    dma_pool_destroy(h->map_pool);
err_map_pool:
    dma_pool_destroy(h->page_pool);
err_page_pool:
err_mask:
    pci_clear_master(p);
    pci_iounmap(p, h->bar0);
err_iomap:
    pci_release_regions(p);
err_request:
    pci_disable_device(p);
err_enable:
    kfree(h);
err_kmalloc:
    return err;
}

static void hd_deinit_pci_dev(struct pci_dev *p) {
    struct hd_dev *h;

    h = pci_get_drvdata(p);

    hd_turn_off(h);
    free_irq(p->irq, h);
    dma_pool_destroy(h->map_pool);
    dma_pool_destroy(h->page_pool);
    pci_clear_master(p);
    pci_iounmap(p, h->bar0);
    pci_release_regions(p);
    pci_disable_device(p);
    kfree(h);
}

static int hd_probe(struct pci_dev *p, const struct pci_device_id *id)
{
    int err;
    int minor;
    struct hd_dev *h;
    struct device *d;
    (void) id;

    minor = getminor();
    if (minor < 0) errjmp2(err = minor, err_minor);

    err = hd_init_pci_dev(p);
    if (err) errjmp(err_pci);

    h = pci_get_drvdata(p);

    cdev_init(&h->cdev, &doom_fops);

    err = cdev_add(&h->cdev, hd_major + minor, 1);
    if (err) errjmp(err_add);

    d = device_create(&hd_class, &p->dev, h->cdev.dev, NULL, "doom%d", minor);
    if (IS_ERR(d)) errjmp2(err = PTR_ERR(d), err_create);

    h->device = d; // Not needed?

    return 0;

err_create:
    cdev_del(&h->cdev);
err_add:
    hd_deinit_pci_dev(p);
err_pci:
    putminor(minor);
err_minor:
    return err;
}

static void hd_remove(struct pci_dev *p)
{
    struct hd_dev *h;

    h = pci_get_drvdata(p);

    device_destroy(&hd_class, h->cdev.dev);
    cdev_del(&h->cdev);
    putminor(MINOR(h->cdev.dev));

    kref_put(&h->refcount, hd_release);
    wait_for_completion(&h->remove_compl);

    hd_deinit_pci_dev(p);
}

static int hd_suspend(struct pci_dev *p, pm_message_t state)
{
    struct hd_dev *h;

    /* All relevant user processes are sleeping by this point. */
    h = pci_get_drvdata(p);
    hd_sync(h);
    hd_turn_off(h);

    return 0;
}

static int hd_resume(struct pci_dev *p)
{
    struct hd_dev *h;

    h = pci_get_drvdata(p);
    hd_turn_on(h);

    return 0;
}


static struct pci_device_id hd_pci_ids[] = {
    { PCI_DEVICE(HARDDOOM_VENDOR_ID, HARDDOOM_DEVICE_ID), },
    { 0, },
};

static struct pci_driver hd_pci_driver = {
    .name = "HardDoom",
    .id_table = hd_pci_ids,
    .probe = hd_probe,
    .remove = hd_remove,
    .suspend = hd_suspend,
    .resume = hd_resume,
};


int hd_init(void)
{
    int err = 0;

    err = class_register(&hd_class);
    if (err) errjmp(err_class);

    err = alloc_chrdev_region(&hd_major, 0, MAX_DEV_NUM, "HardDoom");
    if (err) errjmp(err_region);

    err = pci_register_driver(&hd_pci_driver);
    if (err) errjmp(err_driver);

    return 0;

err_driver:
    unregister_chrdev_region(hd_major, MAX_DEV_NUM);
err_region:
    class_unregister(&hd_class);
err_class:
    return err;
}

void hd_exit(void)
{
    pci_unregister_driver(&hd_pci_driver);
    unregister_chrdev_region(hd_major, MAX_DEV_NUM);
    class_unregister(&hd_class);
}

module_init(hd_init);
module_exit(hd_exit);
