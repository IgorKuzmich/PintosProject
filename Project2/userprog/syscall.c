#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "threads/malloc.h"
#include "userprog/process.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "devices/shutdown.h"
#include "devices/input.h"

static void syscall_handler (struct intr_frame *);
static int get_user (const uint8_t *uaddr);
static bool put_user (uint8_t *udst, uint8_t byte);
static bool is_ptr_valid(const void *ptr);
static bool is_buffer_valid(int8_t *begin, int8_t *end);
static bool is_string_valid(const char* s);

//static bool check_user_buffer(const void *ptr, size_t n);

static bool is_ptr_valid_for_n(const void *ptr, size_t n);
uint32_t get_user_4bytes(const uint8_t *uaddr);
static void get_user_error(void);

static void halt (void) NO_RETURN;
void exit(int status) NO_RETURN ;
static int write(int fd, const void *buffer, unsigned size);
static pid_t exec(const char *filename);
static int wait (pid_t pid);

static bool create (const char *file, unsigned initial_size);
static bool remove (const char *file);
static int open (const char *file);
static void close (int fd);
static int read (int fd, void *buffer, unsigned size);
static int filesize (int fd);
static void seek (int fd, unsigned position);
unsigned tell (int fd);

static void get_argv(struct intr_frame *f, uint32_t *argv,int argc);
static struct child_thread * get_child(pid_t pid);

static void
syscall_handler (struct intr_frame *f) 
{
  if(is_ptr_valid_for_n(f->esp, 4))
  {
    switch(get_user_4bytes(f->esp))
    {
      case SYS_HALT:                   /* Halt the operating system. */
        halt();
        break;
      case SYS_EXIT:                  /* Terminate this process. */
      {
        //printf("*****IN EXIT\n");
        uint32_t argv[1];
        get_argv(f, argv, 1);
        exit((int)argv[0]);
        break;
      }
      case SYS_EXEC:                   /* Start another process. */
      {
        lock_acquire(&filesys_lock);
        //printf("*****IN EXEC\n");
        uint32_t argv[1];
        get_argv(f, argv, 1);
        f->eax = exec((const char *) argv[0]);
        lock_release(&filesys_lock);
        break;
      }
      case SYS_WAIT:                   /* Wait for a child process to die. */
      {
        //printf("*****IN WAIT\n");
        uint32_t argv[1];
        get_argv(f, argv, 1);
        f->eax = wait((pid_t) argv[0]);
        break;
      }
      case SYS_CREATE:                 /* Create a file. */
      {
        lock_acquire(&filesys_lock);
        uint32_t argv[2];
        get_argv(f, argv, 2);
        f->eax = create((const char *)argv[0], (unsigned) argv[1]);
        lock_release(&filesys_lock);
        break;
      }
      case SYS_REMOVE:                 /* Delete a file. */
      {
        lock_acquire(&filesys_lock);
        uint32_t argv[1];
        get_argv(f, argv, 1);
        f->eax = remove((const char *)argv[0]);
        lock_release(&filesys_lock);
        break;
      }
      case SYS_OPEN:                   /* Open a file. */
      {
        lock_acquire(&filesys_lock);
        uint32_t argv[1];
        get_argv(f, argv, 1);
        f->eax = open((const char *) argv[0]);
        lock_release(&filesys_lock);
        break;
      }
      case SYS_FILESIZE:               /* Obtain a file's size. */
      {
        lock_acquire(&filesys_lock);
        uint32_t argv[1];
        get_argv(f, argv, 1);
        f->eax = filesize((int) argv[0]);
        lock_release(&filesys_lock);
        break;
      }
      case SYS_READ:                   /* Read from a file. */
      {
        lock_acquire(&filesys_lock);
        uint32_t argv[3];
        get_argv(f, argv, 3);
        f->eax = read((int)argv[0], (void*)argv[1], (unsigned)argv[2]);
        lock_release(&filesys_lock);
        break;
      }
      case SYS_WRITE:                  /* Write to a file. */
      {
        lock_acquire(&filesys_lock);
        uint32_t argv[3];
        get_argv(f, argv, 3);
        f->eax = write((int) argv[0], (const void *) argv[1], (unsigned) argv[2]);
        lock_release(&filesys_lock);
        break;
      }
      case SYS_SEEK:                   /* Change position in a file. */
        {
        lock_acquire(&filesys_lock);
        uint32_t argv[2];
        get_argv(f, argv, 2);
        seek((int)argv[0], (unsigned)argv[1]);
        lock_release(&filesys_lock);
        break;
      }
      case SYS_TELL:                   /* Report current position in a file. */
      {
        lock_acquire(&filesys_lock);
        uint32_t argv[1];
        get_argv(f, argv, 1);
        f->eax = tell((int)argv[0]);
        lock_release(&filesys_lock);
        break;
      }
      case SYS_CLOSE:
      {
        lock_acquire(&filesys_lock);
        uint32_t argv[1];
        get_argv(f, argv, 1);
        close((int) argv[0]);
        lock_release(&filesys_lock);
        break;
      }
    }
  }
  else
  {
    exit(-1);
  }
}

unsigned tell (int fd)
{
  struct thread *t = thread_current();
  struct list_elem *e;
  for(e = list_begin(&t->fd_list); e != list_end(&t->fd_list); e = list_next(e))
  {
    struct fd * f = list_entry(e, struct fd, fd_elem);
    if(f->fd == fd)
    {
      return file_tell(f->file);
    }
  }
  return -1;
}

static void seek (int fd, unsigned position)
{
  struct thread *t = thread_current();
  struct list_elem *e;
  for(e = list_begin(&t->fd_list); e != list_end(&t->fd_list); e = list_next(e))
  {
    struct fd * f = list_entry(e, struct fd, fd_elem);
    if(f->fd == fd)
    {
      file_seek (f->file, position);
      return;
    }
  }
}

static int filesize (int fd)
{
  if(fd < 2)
  {
    return -1;
  }
  struct thread *t = thread_current();
  struct list_elem *e;
  for(e = list_begin(&t->fd_list); e != list_end(&t->fd_list); e = list_next(e))
  {
    struct fd * f = list_entry(e, struct fd, fd_elem);
    if(f->fd == fd)
    {
      return file_length(f->file);
    }
  }
  return -1;
}

static int read (int fd, void *buffer, unsigned size)
{
  if(buffer == NULL)
  {
    exit(-1);
  }
  if(!is_buffer_valid((int8_t*)buffer, ((int8_t *)buffer)+size))
  {
    exit(-1);
  }
  else
  {
    if(fd == 0)
    {
      for(uint8_t* b = (uint8_t*) buffer; b != ((uint8_t *)buffer)+size; b++)
      {
        int e = put_user(b, input_getc());
        if(e == -1)
        {
          exit(-1);
        }
      }
    }
    else
    {
      struct thread *t = thread_current();
      struct list_elem *e;
      for(e = list_begin(&t->fd_list); e != list_end(&t->fd_list); e = list_next(e))
      {
        struct fd * f = list_entry(e, struct fd, fd_elem);
        if(f->fd == fd)
        {
          return file_read(f->file, buffer, size);
        }
      }
    }
  }
  return -1;
}

static void close (int fd)
{
  if(fd < 2)
  {
    return;
  }
  else
  {
    struct thread *t = thread_current();

    struct list_elem *e;
    for(e = list_begin(&t->fd_list); e != list_end(&t->fd_list); e = list_next(e))
    {
      struct fd * f = list_entry(e, struct fd, fd_elem);
      if(f->fd == fd)
      {
        list_remove(&f->fd_elem);
        file_close(f->file);
        free(f);
        break;
      }
    }
  }
}

static int open (const char *file)
{
  if(file == NULL)
  {
    return -1;
  }
  if((void*)file >= PHYS_BASE)
  {
    return -1;
  }
  if(!is_string_valid(file))
  {
    exit(-1);
  }
  struct file *f = filesys_open(file);

  if(f == NULL)
  {
    return -1;
  }
  
  struct fd * file_d = malloc(sizeof(struct fd));
  
  struct thread *t = thread_current();
  lock_acquire(&t->fd_lock);
  if(list_empty(&t->fd_list))
  {
    file_d->fd = 2;
  }
  else
  {
    file_d->fd = ((list_entry(list_back(&t->fd_list), struct fd, fd_elem))->fd)+1;
  }

  file_d->file = f;
  list_push_back(&t->fd_list, &file_d->fd_elem);
  lock_release(&t->fd_lock);

  return file_d->fd;
}

static bool create(const char *file, unsigned initial_size)
{
  if(file == NULL)
  {
    exit(-1);
  }
  if(!is_string_valid(file))
  {
    exit(-1);
  }
  bool success = filesys_create(file, initial_size);

  return success;
}

static bool remove (const char *file)
{
  if(file == NULL)
  {
    exit(-1);
  }
  if(!is_string_valid(file))
  {
    exit(-1);
  }
  bool success = filesys_remove(file);

  return success;
}

static void halt(void)
{
  shutdown_power_off();
}

static bool is_string_valid(const char* s)
{
  if(s == NULL)
  {
    exit(-1);
  }
  do
  {
    if((void*)s >= PHYS_BASE)
    {
      return false;
    }
    if(get_user((const uint8_t *)s) == -1 )
    {
      return false;
    }
  }
  while(*(s++) != '\0');

  return true;
}

static bool is_buffer_valid(int8_t *begin, int8_t *end)
{
  for(;begin != end; begin++)
  {
    if((void*)begin >= PHYS_BASE)
    {
      return false;
    }
    if(get_user((const uint8_t *)begin) == -1 )
    {
      return false;
    }
  }
  return true;
}

static struct child_thread* get_child(pid_t pid)
{
  struct thread *t = thread_current();

  lock_acquire(&t->child_lock);
  struct list_elem *e;

  for(e = list_begin(&t->child_list); e != list_end(&t->child_list); e = list_next(e))
  {
    struct child_thread* temp = list_entry (e, struct child_thread, child_elem);
    if(temp->pid == pid)
    {
      lock_release(&t->child_lock);
      return temp;
    }
  }

  lock_release(&t->child_lock);
  return NULL;
}

static int wait (pid_t pid)
{
  return process_wait((tid_t)pid);
}

static pid_t exec(const char *filename)
{
  if(filename == NULL)
  {
    exit(-1);
  }
  if(!is_string_valid(filename))
  {
    exit(-1);
  }

  tid_t pid = process_execute(filename);
  struct child_thread *ct = get_child(pid);

  if(ct->successful_load)
  {
    return pid;
  }
  else
  {
    struct thread * t = thread_current();
    lock_acquire(&t->child_lock);
    list_remove(&ct->child_elem);
    free(ct);
    lock_release(&t->child_lock);
    return -1;
  }
}

static void get_user_error(void)
{
  exit(-1);
}

uint32_t get_user_4bytes(const uint8_t *uaddr)
{
  union s
  {
    uint32_t integer;
    uint8_t bytes[4];
  } n;

  for(size_t i = 0; i < 4; i++)
  {
    int byte = get_user(uaddr + i);
    if(byte == -1)
    {
      get_user_error();
    }
    else
    {
      n.bytes[i] = (char) byte; 
    }
  }
  return n.integer;
}

//returns true if pointer is below phys base
inline static bool is_ptr_valid(const void *ptr)
{
  char* bptr = (char*)ptr;
  if(bptr < (char*)PHYS_BASE && (get_user((const uint8_t*)bptr) != -1))
  {
    return true;
  }
  else
  {
    return false;
  }
}

static bool is_ptr_valid_for_n(const void *ptr, size_t n)
{
  for(unsigned int i = 0; i < n; i++)
  {
    if(!is_ptr_valid(ptr + i))
    {
      return false;
    }
  }
  return true;
}

/* Reads a byte at user virtual address UADDR.
UADDR must be below PHYS_BASE.
Returns the byte value if successful, -1 if a segfault
occurred. */
static int
get_user (const uint8_t *uaddr)
{
   int result;
   asm ("movl $1f, %0; movzbl %1, %0; 1:"
      : "=&a" (result) : "m" (*uaddr));
   return result;
}

/* Writes BYTE to user address UDST.
UDST must be below PHYS_BASE.
Returns true if successful, false if a segfault occurred. */
static bool
put_user (uint8_t *udst, uint8_t byte)
{
   int error_code;
   asm ("movl $1f, %0; movb %b2, %1; 1:"
      : "=&a" (error_code), "=m" (*udst) : "q" (byte));
   return error_code != -1;
}

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

void exit(int status)
{

  struct thread* t = thread_current();

  if(t->parent != NULL)
  {
    struct child_thread *ct = t->child_struct;
    lock_acquire(&ct->child_thread_lock);

    ct->exit_status = status;

    sema_up(&ct->wait_till_exit);

    lock_release(&ct->child_thread_lock);
  }

  printf ("%s: exit(%d)\n", thread_current()->name, status);
  thread_exit();
}

static int write(int fd, const void *buffer, unsigned size)
{
  if(buffer == NULL)
  { 
    exit(-1);
  }
  if(!is_buffer_valid((int8_t*)buffer, ((int8_t *)buffer)+size))
  {
    exit(-1);
  }
  else
  {
    if(fd == STDOUT_FILENO)
    {
      putbuf(buffer, size);
      return size;
    }
    else
    {
      struct thread *t = thread_current();
      struct list_elem *e;
      for(e = list_begin(&t->fd_list); e != list_end(&t->fd_list); e = list_next(e))
      {
        struct fd * f = list_entry(e, struct fd, fd_elem);
        if(f->fd == fd)
        {
          return file_write(f->file, buffer, size);
        }
      }
    }
  }
  return -1;
}

/*static bool check_user_buffer(const void *ptr, size_t n)
{
  if(!is_ptr_valid_for_n(ptr, n/4))
  {
    //printf("*buffer below PHYS_BASE!\n");
    return false;
  }
  else
  {
    size_t size = n;
    for(size_t i = 0; i < size; i++)
    {
      int byte = get_user(((const uint8_t*) ptr) + i);
      if(byte == -1)
      {
        return false;
      }
    }
  }
  return true;
}*/

static void get_argv(struct intr_frame *f, uint32_t *argv,int argc)
{
  for(int i = 0; i < argc; i++)
  {
    if(is_ptr_valid(f->esp + 4*i + 4) && is_ptr_valid(f->esp + 4*i + 3))
    {
      argv[i] = get_user_4bytes((const uint8_t*)((f->esp) + (4*i) + 4));
    }
    else
    {
      exit(-1);
    }
  }
}