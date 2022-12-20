#include "state.h"
#include "betterassert.h"
#include "utils.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Persistent FS state
 * (in reality, it should be maintained in secondary memory;
 * for simplicity, this project maintains it in primary memory).
 */
static tfs_params fs_params;

// Inode table
static inode_t *inode_table;
static pthread_rwlock_t *inode_rwl;
static allocation_state_t *freeinode_ts;
static pthread_rwlock_t inode_table_rwl;

// Data blocks
static char *fs_data; // # blocks * block size
static allocation_state_t *free_blocks;
static pthread_rwlock_t free_blocks_rwl;

/*
 * Volatile FS state
 */
static open_file_entry_t *open_file_table;
static allocation_state_t *free_open_file_entries;
static pthread_rwlock_t open_file_table_rwl;

// Convenience macros
#define INODE_TABLE_SIZE (fs_params.max_inode_count)
#define DATA_BLOCKS (fs_params.max_block_count)
#define MAX_OPEN_FILES (fs_params.max_open_files_count)
#define BLOCK_SIZE (fs_params.block_size)
#define MAX_DIR_ENTRIES (BLOCK_SIZE / sizeof(dir_entry_t))

static inline bool valid_inumber(int inumber) {
    return inumber >= 0 && inumber < INODE_TABLE_SIZE;
}

static inline bool valid_block_number(int block_number) {
    return block_number >= 0 && block_number < DATA_BLOCKS;
}

static inline bool valid_file_handle(int file_handle) {
    return file_handle >= 0 && file_handle < MAX_OPEN_FILES;
}

size_t state_block_size(void) { return BLOCK_SIZE; }

/**
 * Do nothing, while preventing the compiler from performing any optimizations.
 *
 * We need to defeat the optimizer for the insert_delay() function.
 * Under optimization, the empty loop would be completely optimized away.
 * This function tells the compiler that the assembly code being run (which is
 * none) might potentially change *all memory in the process*.
 *
 * This prevents the optimizer from optimizing this code away, because it does
 * not know what it does and it may have side effects.
 *
 * Reference with more information: https://youtu.be/nXaxk27zwlk?t=2775
 *
 * Exercise: try removing this function and look at the assembly generated to
 * compare.
 */
static void touch_all_memory(void) { __asm volatile("" : : : "memory"); }

/**
 * Artifically delay execution (busy loop).
 *
 * Auxiliary function to insert a delay.
 * Used in accesses to persistent FS state as a way of emulating access
 * latencies as if such data structures were really stored in secondary memory.
 */
static void insert_delay(void) {
    for (int i = 0; i < DELAY; i++) {
        touch_all_memory();
    }
}

/**
 * Initialize FS state.
 *
 * Input:
 *   - params: TécnicoFS parameters
 *
 * Returns 0 if successful, -1 otherwise.
 *
 * Possible errors:
 *   - TFS already initialized.
 *   - malloc failure when allocating TFS structures.
 */
int state_init(tfs_params params) {
    fs_params = params;

    if (inode_table != NULL) {
        return -1; // already initialized
    }

    inode_table = malloc(INODE_TABLE_SIZE * sizeof(inode_t));
    inode_rwl = malloc(INODE_TABLE_SIZE * sizeof(pthread_rwlock_t));
    freeinode_ts = malloc(INODE_TABLE_SIZE * sizeof(allocation_state_t));
    fs_data = malloc(DATA_BLOCKS * BLOCK_SIZE);
    free_blocks = malloc(DATA_BLOCKS * sizeof(allocation_state_t));
    open_file_table = malloc(MAX_OPEN_FILES * sizeof(open_file_entry_t));
    free_open_file_entries =
        malloc(MAX_OPEN_FILES * sizeof(allocation_state_t));

    if (!inode_table || !freeinode_ts || !fs_data || !free_blocks ||
        !open_file_table || !free_open_file_entries) {
        return -1; // allocation failed
    }

    for (size_t i = 0; i < INODE_TABLE_SIZE; i++) {
        freeinode_ts[i] = FREE;
    }

    for (size_t i = 0; i < DATA_BLOCKS; i++) {
        free_blocks[i] = FREE;
    }

    for (size_t i = 0; i < MAX_OPEN_FILES; i++) {
        free_open_file_entries[i] = FREE;
    }

    rwl_init(&inode_table_rwl);
    rwl_init(&free_blocks_rwl);
    rwl_init(&open_file_table_rwl);

    return 0;
}

/**
 * Destroy FS state.
 *
 * Returns 0 if succesful, -1 otherwise.
 */
int state_destroy(void) {

    // destroy all remaining inode locks
    for (int i = 0; i < INODE_TABLE_SIZE; ++i) {
        if(freeinode_ts[i] == TAKEN) {
            rwl_destroy(inode_rwl + i);
        }
    }

    free(inode_table);
    free(inode_rwl);
    free(freeinode_ts);
    free(fs_data);
    free(free_blocks);
    free(open_file_table);
    free(free_open_file_entries);

    inode_table = NULL;
    freeinode_ts = NULL;
    fs_data = NULL;
    free_blocks = NULL;
    open_file_table = NULL;
    free_open_file_entries = NULL;

    rwl_destroy(&inode_table_rwl);
    rwl_destroy(&free_blocks_rwl);
    rwl_destroy(&open_file_table_rwl);

    return 0;
}

/**
 * (Try to) Allocate a new inode in the inode table, without initializing its
 * data.
 *
 * Returns the inumber of the newly allocated inode, or -1 in the case of error.
 *
 * Possible errors:
 *   - No free slots in inode table.
 */
static int inode_alloc(void) {
    rwl_wrlock(&inode_table_rwl);
    for (size_t inumber = 0; inumber < INODE_TABLE_SIZE; inumber++) {
        if ((inumber * sizeof(allocation_state_t) % BLOCK_SIZE) == 0) {
            insert_delay(); // simulate storage access delay (to freeinode_ts)
        }

        // Finds first free entry in inode table
        if (freeinode_ts[inumber] == FREE) {
            freeinode_ts[inumber] = TAKEN;
            rwl_unlock(&inode_table_rwl);
            return (int)inumber;
        }
    }

    rwl_unlock(&inode_table_rwl);
    // no free inodes
    return -1;
}

/**
 * Create a new inode in the inode table.
 *
 * Allocates and initializes a new inode.
 * Directories will have their data block allocated and initialized, with i_size
 * set to BLOCK_SIZE. Regular files will not have their data block allocated
 * (i_size will be set to 0, i_data_block to -1).
 *
 * Input:
 *   - i_type: the type of the node (file or directory)
 *
 * Returns inumber of the new inode, or -1 in the case of error.
 *
 * Possible errors:
 *   - No free slots in inode table.
 *   - (if creating a directory) No free data blocks.
 */
int inode_create(inode_type i_type) {
    int inumber = inode_alloc();
    if (inumber == -1) {
        return -1; // no free slots in inode table
    }

    /*
    * section doesn't require locks as inode_alloc returned a thread-safe inum
    */

    inode_t *inode = &inode_table[inumber];
    insert_delay(); // simulate storage access delay (to inode)

    inode->i_node_type = i_type;
    inode->i_links_count = 1;
    rwl_init(inode_rwl + inumber);
    switch (i_type) {
    case T_DIRECTORY: {
        // Initializes directory (filling its block with empty entries, labeled
        // with inumber==-1)
        int b = data_block_alloc();
        if (b == -1) {
            // ensure fields are initialized
            inode->i_size = 0;
            inode->i_data_block = -1;

            // run regular deletion process
            inode_delete(inumber);
            return -1;
        }

        inode_table[inumber].i_size = BLOCK_SIZE;
        inode_table[inumber].i_data_block = b;

        dir_entry_t *dir_entry = (dir_entry_t *)data_block_get(b);
        if (dir_entry == NULL) {
            return -1;
        }

        for (size_t i = 0; i < MAX_DIR_ENTRIES; i++) {
            dir_entry[i].d_inumber = -1;
        }
    } break;
    case T_FILE:
        // In case of a new file, simply sets its size to 0
        inode_table[inumber].i_size = 0;
        inode_table[inumber].i_data_block = -1;
        break;
    case T_LINK:
        // Size unused because no read and write operations in link type files
        inode_table[inumber].i_size = 0;
        inode_table[inumber].i_data_block = -1;
        break;
    default:
        PANIC("inode_create: unknown file type");
    }

    return inumber;
}

/**
 * Delete an inode.
 *
 * Input:
 *   - inumber: inode's number
 * Returns 0 if succesful and -1 on error (possible errors might be another
 * thread deleting this inode)
 */
int inode_delete(int inumber) {
    // simulate storage access delay (to inode and freeinode_ts)
    insert_delay();
    insert_delay();

    if (!valid_inumber(inumber)) {
        return -1;
    }

    // lock inode table
    rwl_wrlock(&inode_table_rwl);
    // if another thread deleted this inode before acquiring rwlock 
    if (freeinode_ts[inumber] == FREE) {
        rwl_unlock(&inode_table_rwl);
        return -1;
    }

    if (inode_table[inumber].i_size > 0) {
        data_block_free(inode_table[inumber].i_data_block);
    }
    
    // destroy inode's rwlock
    rwl_destroy(inode_rwl + inumber);

    freeinode_ts[inumber] = FREE;

    rwl_unlock(&inode_table_rwl);

    return 0;
}


/**
 * Obtain a pointer to an inode from its inumber.
 *
 * Input:
 *   - inumber: inode's number
 *
 * Returns pointer to inode.
 */
inode_t *inode_get(int inumber) {
    if (!valid_inumber(inumber)) {
        return NULL;
    }

    insert_delay(); // simulate storage access delay to inode
    return &inode_table[inumber];
}

pthread_rwlock_t *inode_rwl_get(int inumber) {
    if (!valid_inumber(inumber)) {
        return NULL;
    }

    return inode_rwl + inumber;
}

/**
 * Clear the directory entry associated with a sub file.
 *
 * Input:
 *   - inode: directory inode
 *   - sub_name: sub file name
 *
 * Returns 0 if successful, -1 otherwise.
 *
 * Possible errors:
 *   - inode is not a directory inode.
 *   - Directory does not contain an entry for sub_name.
 */
int clear_dir_entry(inode_t *inode, char const *sub_name) {
    insert_delay();
    // if not a directory
    if (inode->i_node_type != T_DIRECTORY) {
        return -1;
    }
    
    // rwlock_wrlock();

    // Locates the block containing the entries of the directory
    dir_entry_t *dir_entry = (dir_entry_t *)data_block_get(inode->i_data_block);
    ALWAYS_ASSERT(dir_entry != NULL,
                  "clear_dir_entry: directory must have a data block");

    for (size_t i = 0; i < MAX_DIR_ENTRIES; i++) {
        if (!strcmp(dir_entry[i].d_name, sub_name)) {
            dir_entry[i].d_inumber = -1;
            memset(dir_entry[i].d_name, 0, MAX_FILE_NAME);
            
            return 0;
        }
    }
    
    return -1; // sub_name not found
}

/**
 * Store the inumber for a sub file in a directory.
 *
 * Input:
 *   - inode: directory inode
 *   - sub_name: sub file name
 *   - sub_inumber: inumber of the sub inode
 *
 * Returns 0 if successful, -1 otherwise.
 *
 * Possible errors:
 *   - inode is not a directory inode.
 *   - sub_name is not a valid file name (length 0 or > MAX_FILE_NAME - 1).
 *   - Directory is already full of entries.
 */
int add_dir_entry(inode_t *inode, char const *sub_name, int sub_inumber) {
    if (strlen(sub_name) == 0 || strlen(sub_name) > MAX_FILE_NAME - 1) {
        return -1; // invalid sub_name
    }

    insert_delay(); // simulate storage access delay to inode with inumber
    if (inode->i_node_type != T_DIRECTORY) {
        return -1; // not a directory
    }

    // Locates the block containing the entries of the directory
    dir_entry_t *dir_entry = (dir_entry_t *)data_block_get(inode->i_data_block);
    ALWAYS_ASSERT(dir_entry != NULL,
                  "add_dir_entry: directory must have a data block");

    // Finds and fills the first empty entry
    for (size_t i = 0; i < MAX_DIR_ENTRIES; i++) {
        if (dir_entry[i].d_inumber == -1) {
            dir_entry[i].d_inumber = sub_inumber;
            strncpy(dir_entry[i].d_name, sub_name, MAX_FILE_NAME - 1);
            dir_entry[i].d_name[MAX_FILE_NAME - 1] = '\0';

            return 0;
        }
    }

    return -1; // no space for entry
}

/**
 * Obtain the inumber for a sub file inside a directory.
 *
 * Input:
 *   - inode: directory inode
 *   - sub_name: sub file name
 *
 * Returns inumber linked to the target name, -1 if errors occur.
 *
 * Possible errors:
 *   - inode is not a directory inode.
 *   - Directory does not contain a file named sub_name.
 */
int find_in_dir(inode_t *inode, char const *sub_name) {
    insert_delay(); // simulate storage access delay to inode with inumber
    if (inode->i_node_type != T_DIRECTORY) {
        return -1; // not a directory
    }

    dir_entry_t *dir_entry = (dir_entry_t *)data_block_get(inode->i_data_block);
    if (dir_entry == NULL) {
        return -1;
    }

    // Iterates over the directory entries looking for one that has the target
    // name
    for (int i = 0; i < MAX_DIR_ENTRIES; i++)
        if ((dir_entry[i].d_inumber != -1) &&
            (strncmp(dir_entry[i].d_name, sub_name, MAX_FILE_NAME) == 0)) {

            int sub_inumber = dir_entry[i].d_inumber;
            return sub_inumber;
        }

    return -1; // entry not found
}

/**
 * Allocate a new data block.
 *
 * Returns block number/index if successful, -1 otherwise.
 *
 * Possible errors:
 *   - No free data blocks.
 */
int data_block_alloc(void) {
    rwl_wrlock(&free_blocks_rwl);
    for (size_t i = 0; i < DATA_BLOCKS; i++) {
        if (i * sizeof(allocation_state_t) % BLOCK_SIZE == 0) {
            insert_delay(); // simulate storage access delay to free_blocks
        }

        if (free_blocks[i] == FREE) {
            free_blocks[i] = TAKEN;
            rwl_unlock(&free_blocks_rwl);
            return (int)i;
        }
    }

    rwl_unlock(&free_blocks_rwl);
    return -1;
}

/**
 * Free a data block.
 *
 * Input:
 *   - block_number: the block number/index
 */
int data_block_free(int block_number) {
    if (!valid_block_number(block_number)) {
        return -1;
    }

    // lock blocks bitmap table (not necessary but might make data more compact)
    rwl_wrlock(&free_blocks_rwl);
    
    insert_delay(); // simulate storage access delay to free_blocks

    free_blocks[block_number] = FREE;
    
    // lock blocks bitmap table
    rwl_unlock(&free_blocks_rwl);

    return 0;
}

/**
 * Obtain a pointer to the contents of a given block.
 *
 * Input:
 *   - block_number: the block number/index
 *
 * Returns a pointer to the first byte of the block.
 */
void *data_block_get(int block_number) {
    if (!valid_block_number(block_number)) {
        return NULL;
    }

    insert_delay(); // simulate storage access delay to block
    return &fs_data[(size_t)block_number * BLOCK_SIZE];
}

/**
 * Add a new entry to the open file table.
 *
 * Input:
 *   - inumber: inode number of the file to open
 *   - offset: initial offset
 *
 * Returns file handle if successful, -1 otherwise.
 *
 * Possible errors:
 *   - No space in open file table for a new open file.
 */
int add_to_open_file_table(int inumber, size_t offset) {

    rwl_wrlock(&open_file_table_rwl);

    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (free_open_file_entries[i] == FREE) {
            free_open_file_entries[i] = TAKEN;
            mutex_lock(&open_file_table[i].lock);
            open_file_table[i].of_inumber = inumber;
            open_file_table[i].of_offset = offset;
            mutex_unlock(&open_file_table[i].lock);
            rwl_unlock(&open_file_table_rwl);
            return i;
        }
    }
    
    rwl_unlock(&open_file_table_rwl);

    return -1;
}

/**
 * Free an entry from the open file table.
 *
 * Input:
 *   - fhandle: file handle to free/close
 * 
 * Returns 0 if succesful and -1 if failed (invalid fhandle or fhandle not in
 * open file table)
 */
int remove_from_open_file_table(int fhandle) {
    // invalid fhandle 
    if(!valid_file_handle(fhandle)) {
        return -1;
    }

    rwl_wrlock(&open_file_table_rwl);
    // not opened fhandle 
    if (free_open_file_entries[fhandle] == FREE) {
        rwl_unlock(&open_file_table_rwl);
        return -1;
    }

    free_open_file_entries[fhandle] = FREE;
    
    // unlock open file table
    rwl_unlock(&open_file_table_rwl);

    return 0;
}

/**
 * Obtain pointer to a given entry in the open file table.
 *
 * Input:
 *   - fhandle: file handle
 *
 * Returns pointer to the entry, or NULL if the fhandle is invalid/closed/never
 * opened.
 */
open_file_entry_t *get_open_file_entry(int fhandle) {
    if (!valid_file_handle(fhandle)) {
        return NULL;
    }

    if (free_open_file_entries[fhandle] == FREE) {
        return NULL;
    }

    return &open_file_table[fhandle];
}

/**
 * Determine if given inumber is in the open file table
 * 
 * Input:
 *   - inumber: inumber of inode to be searched for in open file table
 * 
 * Returns true if given inumber is present in the open file table and false
 * if not.
 * 
*/
bool is_in_open_file_table(int inumber) {
    if (!valid_inumber(inumber)) {
        return false;
    }

    rwl_rdlock(&open_file_table_rwl);

    for (int i = 0; i < MAX_OPEN_FILES; ++i) {
        if (free_open_file_entries[i] == TAKEN) {
            if (open_file_table[i].of_inumber == inumber) {
                rwl_unlock(&open_file_table_rwl);
                return true;
            }
        }
    }

    rwl_unlock(&open_file_table_rwl);

    return false;
}
