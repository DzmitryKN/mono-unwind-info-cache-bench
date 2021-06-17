#pragma once
typedef unsigned int guint32;
typedef unsigned short guint16;
typedef unsigned char guint8;
typedef void *gpointer;
#define g_free free
#define g_malloc malloc
#define g_assert assert
#define g_new0(t, n) (t *)calloc(sizeof(t), n)
#define MONO_ZERO_LEN_ARRAY 0

