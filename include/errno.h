#ifndef _CINDER_ERRNO_H
#define _CINDER_ERRNO_H

// On the Microsoft runtime errno is not a variable but a function returning a
// pointer to the thread's own copy, which is what makes it thread-safe.
int *_errno(void);
#define errno (*_errno())

#define EPERM 1
#define ENOENT 2
#define EINTR 4
#define EIO 5
#define EBADF 9
#define ENOMEM 12
#define EACCES 13
#define EEXIST 17
#define EINVAL 22
#define ERANGE 34

#endif
