/* Single-Image implementation of Libcaf

Copyright (c) 2012-2014, OpenCoarray Consortium
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the <organization> nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.  */

#include "libcaf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>	/* For memcpy.  */
#include <stdarg.h>	/* For variadic arguments.  */
#include <message.h>    /* ARMCI and armci_msg_*.  */


/* Define GFC_CAF_CHECK to enable run-time checking.  */
/* #define GFC_CAF_CHECK  1  */

typedef void ** armci_token_t;
#define TOKEN(X) ((armci_token_t) (X))


static void error_stop (int error) __attribute__ ((noreturn));

/* Global variables.  */
static int caf_this_image;
static int caf_num_images;
static int caf_is_finalized;

caf_static_t *caf_static_list = NULL;

static int **arrived;
static int *orders;
static int sizeOrders = 0;
static int *images_full;

/* Keep in sync with single.c.  */
static void
caf_runtime_error (const char *message, ...)
{
  va_list ap;
  fprintf (stderr, "Fortran runtime error on image %d: ", caf_this_image);
  va_start (ap, message);
  vfprintf (stderr, message, ap);
  va_end (ap);
  fprintf (stderr, "\n");

  /* FIXME: Shutdown the Fortran RTL to flush the buffer.  PR 43849.  */
  /* FIXME: Do some more effort than just to abort.  */
  armci_msg_abort (EXIT_FAILURE);

  /* Should be unreachable, but to make sure also call exit.  */
  exit (EXIT_FAILURE);
}


/* Initialize coarray program.  This routine assumes that no other
   ARMCI initialization happened before. */

void
PREFIX(init) (int *argc, char ***argv)
{
  if (caf_num_images == 0)
    {
      int ierr,i=0,j=0;

      armci_msg_init (argc, argv);
      if (unlikely ((ierr = ARMCI_Init()) != 0))
	caf_runtime_error ("Failure when initializing ARMCI: %d", ierr);

      caf_num_images = armci_msg_nproc ();
      caf_this_image = armci_msg_me ();
      caf_this_image++;
      caf_is_finalized = 0;

      images_full = (int *)calloc(caf_num_images-1,sizeof(int));

      ierr = ARMCI_Create_mutexes(1);

      for(i=0;i<caf_num_images;i++)
      	{
      	  if(i+1 != caf_this_image)
      	    {
      	      images_full[j]=i+1;
      	      j++;
      	    }
      	}
    }
}


/* Finalize coarray program.   */

void
PREFIX(finalize) (void)
{
  while (caf_static_list != NULL)
    {
      caf_static_t *tmp = caf_static_list->prev;

      (void) ARMCI_Free (TOKEN(caf_static_list->token)[caf_this_image-1]);
      free (TOKEN(caf_static_list->token));
      free (caf_static_list);
      caf_static_list = tmp;
    }

  (void) ARMCI_Finalize ();
  armci_msg_finalize ();

  caf_is_finalized = 1;
}


int
PREFIX(this_image) (int distance __attribute__ ((unused)))
{
  return caf_this_image;
}


int
PREFIX(num_images) (int distance __attribute__ ((unused)),
		    int failed __attribute__ ((unused)))
{
  return caf_num_images;
}


void *
PREFIX(register) (size_t size, caf_register_t type, caf_token_t *token,
		  int *stat, char *errmsg, int errmsg_len)
{
  int ierr,i;

  if (unlikely (caf_is_finalized))
    goto error;

  /* Start ARMCI if not already started.  */
  if (caf_num_images == 0)
    PREFIX(init) (NULL, NULL);

  *token = malloc (sizeof (armci_token_t));

  if (*token == NULL)
    goto error;

  *token = malloc (sizeof (void*) * caf_num_images);
  if (TOKEN(*token) == NULL)
    goto error;

  ierr = ARMCI_Malloc (TOKEN(*token), size);

  if (unlikely (ierr))
    {
      free (TOKEN(*token));
      goto error;
    }

  if (type == CAF_REGTYPE_COARRAY_STATIC)
    {
      caf_static_t *tmp = malloc (sizeof (caf_static_t));
      tmp->prev  = caf_static_list;
      tmp->token = *token;
      caf_static_list = tmp;
    }

  if (stat)
    *stat = 0;

  orders = calloc(caf_num_images,sizeof(int));

  arrived = malloc(sizeof(int *) * caf_num_images);

  ierr = ARMCI_Malloc ((void **)arrived,sizeof(int)*caf_num_images);
  
  for(i=0;i<caf_num_images;i++)
    arrived[caf_this_image-1][i] = 0;

  return TOKEN(*token)[caf_this_image-1];

error:
  {
    char *msg;

    if (caf_is_finalized)
      msg = "Failed to allocate coarray - there are stopped images";
    else
      msg = "Failed to allocate coarray";

    if (stat)
      {
	*stat = caf_is_finalized ? STAT_STOPPED_IMAGE : 1;
	if (errmsg_len > 0)
	  {
	    int len = ((int) strlen (msg) > errmsg_len) ? errmsg_len
							: (int) strlen (msg);
	    memcpy (errmsg, msg, len);
	    if (errmsg_len > len)
	      memset (&errmsg[len], ' ', errmsg_len-len);
	  }
      }
    else
      caf_runtime_error (msg);
  }

  return NULL;
}


void
PREFIX(deregister) (caf_token_t *token, int *stat, char *errmsg, int errmsg_len)
{
  int ierr;

  if (unlikely (caf_is_finalized))
    {
      const char msg[] = "Failed to deallocate coarray - "
			  "there are stopped images";
      if (stat)
	{
	  *stat = STAT_STOPPED_IMAGE;
	
	  if (errmsg_len > 0)
	    {
	      int len = ((int) sizeof (msg) - 1 > errmsg_len)
			? errmsg_len : (int) sizeof (msg) - 1;
	      memcpy (errmsg, msg, len);
	      if (errmsg_len > len)
		memset (&errmsg[len], ' ', errmsg_len-len);
	    }
	  return;
	}
      caf_runtime_error (msg);
    }

  PREFIX(sync_all) (NULL, NULL, 0);

  if (stat)
    *stat = 0;

  if (unlikely (ierr = ARMCI_Free (TOKEN(*token)[caf_this_image-1])))
    caf_runtime_error ("ARMCI memory freeing failed: Error code %d", ierr);

  free (TOKEN(*token));
}


void
PREFIX(sync_all) (int *stat, char *errmsg, int errmsg_len)
{
  int ierr;

  if (unlikely (caf_is_finalized))
    ierr = STAT_STOPPED_IMAGE;
  else
    {
      ARMCI_AllFence ();
      armci_msg_barrier ();
      ierr = 0;
    }
 
  if (stat)
    *stat = ierr;

  if (ierr)
    {
      char *msg;
      if (caf_is_finalized)
	msg = "SYNC ALL failed - there are stopped images";
      else
	msg = "SYNC ALL failed";

      if (errmsg_len > 0)
	{
	  int len = ((int) strlen (msg) > errmsg_len) ? errmsg_len
						      : (int) strlen (msg);
	  memcpy (errmsg, msg, len);
	  if (errmsg_len > len)
	    memset (&errmsg[len], ' ', errmsg_len-len);
	}
      else
	caf_runtime_error (msg);
    }
}

/* token: The token of the array to be written to. */
/* offset: Difference between the coarray base address and the actual data, used for caf(3)[2] = 8 or caf[4]%a(4)%b = 7. */
/* image_index: Index of the coarray (typically remote, though it can also be on this_image). */
/* data: Pointer to the to-be-transferred data. */
/* size: The number of bytes to be transferred. */
/* asynchronous: Return before the data transfer has been complete  */

void
PREFIX(send) (caf_token_t token, size_t offset, int image_index, void *data,
	      size_t size, bool async)
{
  int ierr;

  if (!async)
    ierr = ARMCI_Put(data, TOKEN(token)[image_index-1] + offset, size,
		     image_index - 1);
  else
    ierr = ARMCI_NbPut(data, TOKEN(token)[image_index-1] + offset, size,
		       image_index-1, NULL);

  if(ierr != 0)
    error_stop (ierr);
}


/* SYNC IMAGES. Note: SYNC IMAGES(*) is passed as count == -1 while
   SYNC IMAGES([]) has count == 0. Note further that SYNC IMAGES(*)
   is not equivalent to SYNC ALL. */
void
PREFIX(sync_images) (int count, int images[], int *stat, char *errmsg,
		     int errmsg_len)
{
  int ierr=0,i,j,wc,*tmp;
  bool freeToGo = false;

  if (count == 0 || (count == 1 && images[0] == caf_this_image))
    {
      if (stat)
	*stat = 0;
      return;
    }

#ifdef GFC_CAF_CHECK
  {
    int i;

    for (i = 0; i < count; i++)
      if (images[i] < 1 || images[i] > caf_num_images)
	{
	  fprintf (stderr, "COARRAY ERROR: Invalid image index %d to SYNC "
		   "IMAGES", images[i]);
	  error_stop (1);
	}
  }
#endif

  /* FIXME: SYNC IMAGES with a nontrivial argument cannot easily be
     mapped to ARMCI communicators. Thus, exist early with an error message.  */

  /* Handle SYNC IMAGES(*).  */
  if (unlikely (caf_is_finalized))
    ierr = STAT_STOPPED_IMAGE;
  else
    {
      /* Insert orders */
      if(count == -1)
	{
	  for(i=0;i<caf_num_images-1;i++)
	    orders[images_full[i]-1]++;
	  count = caf_num_images-1;
	  images = images_full;
	}
      else
	{
	  for(i=0;i<count;i++)
	    orders[images[i]-1]++;
	}

      /*Sending  ack */
      
      int val;

      for(i=0;i<count;i++)
	{
	  ARMCI_Lock(0, images[i]-1);

	  val = ARMCI_GetValueInt((void *)&arrived[images[i]-1][caf_this_image-1], images[i]-1);
	  val++;
	  ierr = ARMCI_PutValueInt(val,(void *)&arrived[images[i]-1][caf_this_image-1], images[i]-1);
	  ARMCI_Unlock(0, images[i]-1);
	}

      while(!freeToGo)
	{
	  ARMCI_Lock(0, caf_this_image-1);
	  
	  sizeOrders = 0;

	  for(i=0;i<caf_num_images;i++)
	    {
	      if(orders[i] != 0)
		{
		  sizeOrders++;
		  val = ARMCI_GetValueInt((void *)&arrived[caf_this_image-1][i], caf_this_image-1);
		  //val = arrived[caf_this_image-1][i];
		  if(val != 0)
		    {
		      orders[i]--;
		      sizeOrders--;
		      val--;
		      ierr = ARMCI_PutValueInt(val, (void *)&arrived[caf_this_image-1][i], caf_this_image-1);
		    }
		}
	    }
	  
	  if(sizeOrders==0)
	    freeToGo=true;
	  
	  ARMCI_Unlock(0, caf_this_image-1);
	  sched_yield();
	}
      
      freeToGo = false;
    }

  if (stat)
    *stat = ierr;

  if (ierr)
    {
      char *msg;
      if (caf_is_finalized)
	msg = "SYNC IMAGES failed - there are stopped images";
      else
	msg = "SYNC IMAGES failed";

      if (errmsg_len > 0)
	{
	  int len = ((int) strlen (msg) > errmsg_len) ? errmsg_len
						      : (int) strlen (msg);
	  memcpy (errmsg, msg, len);
	  if (errmsg_len > len)
	    memset (&errmsg[len], ' ', errmsg_len-len);
	}
      else
	caf_runtime_error (msg);
    }
}


/* ERROR STOP the other images.  */

static void
error_stop (int error)
{
  /* FIXME: Shutdown the Fortran RTL to flush the buffer.  PR 43849.  */
  /* FIXME: Do some more effort than just ARMCI_Error.  */
  ARMCI_Error (NULL, error);

  /* Should be unreachable, but to make sure also call exit.  */
  exit (error);
}


/* ERROR STOP function for string arguments.  */

void
PREFIX(error_stop_str) (const char *string, int32_t len)
{
  fputs ("ERROR STOP ", stderr);
  while (len--)
    fputc (*(string++), stderr);
  fputs ("\n", stderr);

  error_stop (1);
}


/* ERROR STOP function for numerical arguments.  */

void
PREFIX(error_stop) (int32_t error)
{
  fprintf (stderr, "ERROR STOP %d\n", error);
  error_stop (error);
}
