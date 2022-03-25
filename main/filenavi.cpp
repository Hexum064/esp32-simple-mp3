#include <config.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include <dirent.h>
#include "filenavi.h"

DIR *dr = NULL;
long int target_pos = 0;
long int cur_pos = 0;
char sd_path[1024];
struct dirent *current_file; // Pointer for directory entry

char *get_current_path()
{
    return sd_path;
}

struct dirent *get_current_file()
{
    return current_file;
}

void revert_path()
{
    int i = 0;
    int lastPos = -1;

    for (; i < 1024 || sd_path[i] == NULL; i++)
    {
        // the next i will always be greater than whatever is stored in lastPos
        if (sd_path[i] == '/')
        {
            lastPos = i;
        }
    }

    if (lastPos < 0 || sd_path[lastPos] == NULL)
        return;

    sd_path[lastPos] = NULL;
}

void open_dir(char *name)
{

    if (dr != NULL)
        closedir(dr);

    dr = opendir(name);
}

void navigate_to_target_pos_from_curr_dir()
{
    long int cur_dir = 0;

    if (target_pos < 1)
        return;

    // first look through the files then recurse through the dirs
    while ((current_file = readdir(dr)) != NULL)
    {
        if (current_file->d_type == DT_REG)
        {
            cur_pos++;
#ifdef DEBUG
            printf("Looing at file %s in %s. Pos: %ld, Target: %ld\n", current_file->d_name, sd_path, cur_pos, target_pos);
#endif
            if (cur_pos == target_pos)
            {
#ifdef DEBUG
                printf("File found at pos: %ld\n", cur_pos);
#endif
                return;
            }

#ifdef DEBUG
            printf("Inc cur_pos to %ld\n", cur_pos);
#endif
        }
    }

    rewinddir(dr);

    // DIRs now
    while ((current_file = readdir(dr)) != NULL)
    {
        if (current_file->d_type == DT_DIR)
        {
            cur_dir = telldir(dr);

            strcat(sd_path, "/");
            strcat(sd_path, current_file->d_name);

            open_dir(sd_path);
#ifdef DEBUG
            printf("Navigating to path %s\n", sd_path);
#endif
            navigate_to_target_pos_from_curr_dir();

            if (cur_pos == target_pos)
            {
#ifdef DEBUG
                printf("File found at pos: %ld\n", cur_pos);
#endif
                return;
            }

            revert_path();
#ifdef DEBUG
            printf("Returning to %s\n", sd_path);
#endif
            open_dir(sd_path);
            seekdir(dr, cur_dir + 1);
        }
    }
#ifdef DEBUG
    printf("No more files or dirs in %s\n", sd_path);
#endif
}

void navigate_to_pos()
{
    current_file = NULL;
    strcpy(sd_path, MOUNT_POINT);
    open_dir(sd_path);
    cur_pos = 0;
    navigate_to_target_pos_from_curr_dir();
}

void navigate_to_pos(long int pos)
{
    target_pos = pos;
    navigate_to_pos();
}

void get_next_file()
{
    target_pos++;
    navigate_to_pos();
}

void get_prev_file()
{
    if (target_pos > 0)
    {
        target_pos--;
        navigate_to_pos();
    }
}

bool is_mp3(char *name)
{
    int size = strlen(name);

    if (size < 4) // should at least be '.mp3'
        return false;

    size--; // for 0 offset

    // returns true if the file ends in '.mp3', case-insensitive
    return (name[size] == '3' && (name[size - 1] == 'p' || name[size - 1] == 'P') && (name[size - 2] == 'm' || name[size - 2] == 'M') && name[size - 3] == '.');
}

void get_first_mp3()
{
    navigate_to_pos(1);

    do
    {
        get_next_file();
    } while (current_file != NULL && !is_mp3(current_file->d_name));
}

void get_next_mp3()
{
    long int curr = target_pos;

    do
    {
        get_next_file();
    } while (current_file != NULL && !is_mp3(current_file->d_name));

    if (current_file == NULL)
    {
        // we probably ran out of files so start from the beginning
        get_first_mp3();
        return;
    }
}

void get_prev_mp3()
{
    long int curr = target_pos;

    do
    {
        get_prev_file();
    } while (current_file != NULL && !is_mp3(current_file->d_name));

    if (current_file == NULL)
    {
        // try to go back, we probably ran out of files
        target_pos = curr;
        navigate_to_pos();
        return;
    }
}
