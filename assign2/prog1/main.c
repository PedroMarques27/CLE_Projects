/**
 *  \file main.c
 *
 *  \brief Problem name: Text Processing with Multiprocessing.
 *
 *  The main objective of this program is to process files in order to obtain
 *  the number of words, and the number of words starting with a vowel and ending in
 *  a consonant.
 *
 *  It is optimized by having one dispatcher process splitting the files with text in chunks,
 *  and sending them to the other worker processes to perform the text processing operations.
 *
 *  Design and flow of the dispatcher process:
 * 
 *  1 - Read and process the command line.
 *  2 - Broadcast a message with the maximum number of bytes each chunk will have.
 *  3 - For every file:
 *    3.1 - Obtain chunks and split them to each worker process until there are no
 *    more chunks or no more workers available.
 *    3.2 - Wait for a response with the processing results from each worker that it sent a chunk.
 *    3.3 - Store the processing results obtained from the workers response.
 *  4 - Send a message to the workers alerting there isn't more work to be done.
 *  5 - Print final processing results.
 *  6 - Finalize.
 * 
 *  Design and flow of the worker process:
 *  
 *  1 - Receive a broadcasted message from the dispatcher with the maximum number of bytes of each chunk.
 *  2 - Until the dispatcher says there is work to be done:
 *    2.1 - Wait for data to process.
 *    2.2 - Process the data.
 *    2.2 - Send to the dispatcher the processing results.
 *  3 - Finalize.
 *
 *  \author Mário Silva - May 2022
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <stdbool.h>
#include <string.h>
#include <libgen.h>
#include <mpi.h>

#include "textProcUtils.h"
#include "probConst.h"

/**
 *  \brief Print command usage.
 *
 *  A message specifying how the program should be called is printed.
 *
 *  \param cmdName string with the name of the command
 */
static void printUsage(char *cmdName);

/**
 *  \brief Print results of the text processing.
 *
 *  Operation carried out by the dispatcher process.
 */
void printResults(struct fileData *filesData, int numFiles);

/**
 *  \brief
 *
 *  Design and flow of the dispatcher process:
 * 
 *  1 - Read and process the command line.
 *  2 - Broadcast a message with the maximum number of bytes each chunk will have.
 *  3 - For every file:
 *    3.1 - Obtain chunks and split them to each worker process until there are no
 *    more chunks or no more workers available.
 *    3.2 - Wait for a response with the processing results from each worker that it sent a chunk.
 *    3.3 - Store the processing results obtained from the workers response.
 *  4 - Send a message to the workers alerting there isn't more work to be done.
 *  5 - Print final processing results.
 *  6 - Finalize.
 * 
 *  Design and flow of the worker process:
 *  
 *  1 - Receive a broadcasted message from the dispatcher with the maximum number of bytes of each chunk.
 *  2 - Until the dispatcher says there is work to be done:
 *    2.1 - Wait for data to process.
 *    2.2 - Process the data.
 *    2.2 - Send to the dispatcher the processing results.
 *  3 - Finalize.
 *
 *  \param argc number of words of the command line
 *  \param argv list of words of the command line
 *
 *  \return status of operation
 */
int main(int argc, char *argv[])
{
  int rank, size;
  int workStatus; /* indicates if there is more work to be done or not */
  /** \brief maximum number of bytes per chunk */
  int maxBytesPerChunk = DB; /* default value is used if not in args */
  int i; /* counting variable */

  // MPI
  MPI_Init(&argc, &argv);
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &size);

  /* This program requires at least 2 processes */
  if (size < 2)
  {
    fprintf(stderr, "Requires at least two processes.\n");
    MPI_Finalize();
    return EXIT_FAILURE;
  }

  if (rank == 0)
  {
    struct timespec start, finish; /* time limits */

    /* timer starts */
    clock_gettime(CLOCK_MONOTONIC_RAW, &start); /* begin of measurement */

    /* process command line arguments and set up variables */
    int nFile;          /* counting variable */
    char *fileNames[M]; /* files to be processed (maximum of M) */
    int numFiles = 0;   /* number of files to process */
    int opt;            /* selected option */
    do
    {
      switch ((opt = getopt(argc, argv, "f:n:m:")))
      {
      case 'f': /* file name */
        if (optarg[0] == '-')
        {
          fprintf(stderr, "%s: file name is missing\n", basename(argv[0]));
          printUsage(basename(argv[0]));
          return EXIT_FAILURE;
        }
        if (numFiles == M)
        {
          fprintf(stderr, "%s: can only process %d files at a time\n", basename(argv[0]), M);
          return EXIT_FAILURE;
        }
        fileNames[numFiles++] = optarg;
        break;
      case 'm': /* numeric argument */
        if (atoi(optarg) < MIN)
        {
          fprintf(stderr, "%s: number of bytes must be greater or equal than %d\n", basename(argv[0]), MIN);
          printUsage(basename(argv[0]));
          return EXIT_FAILURE;
        }
        maxBytesPerChunk = (int)atoi(optarg);
        break;
      case 'h': /* help mode */
        printUsage(basename(argv[0]));
        return EXIT_SUCCESS;
      case '?': /* invalid option */
        fprintf(stderr, "%s: invalid option\n", basename(argv[0]));
        printUsage(basename(argv[0]));
        return EXIT_FAILURE;
      case -1:
        break;
      }
    } while (opt != -1);

    if (argc == 1)
    {
      fprintf(stderr, "%s: invalid format\n", basename(argv[0]));
      printUsage(basename(argv[0]));
      return EXIT_FAILURE;
    }


    /* Tell each worker the maximum number of bytes each chunk will have so they can initialize the buffer */
    MPI_Bcast(&maxBytesPerChunk, 1, MPI_INT, 0, MPI_COMM_WORLD);

    struct fileData *filesData = (struct fileData *)malloc(numFiles * sizeof(struct fileData)); /* allocating memory for numFiles of fileData structs */
    int nWorkers = 0;                                                                           /* number of worker processes that got chunks */
    int previousCh = 0;                                                                         /* last character read of the previous chunk */
    int nWords = 0, nWordsBV = 0, nWordsEC = 0;                                                 /* results of the processing received from the workers */
    workStatus = FILES_IN_PROCESSING;                                                           /* work needs to be done */
    unsigned char * chunk = (unsigned char *)malloc(maxBytesPerChunk * sizeof(unsigned char));  /* allocating memory for the chunk buffer */
    memset(chunk, 0, maxBytesPerChunk * sizeof(unsigned char));

    for (nFile = 0; nFile < numFiles; nFile++)
    {
      /* initialize struct data */
      (filesData + nFile)->fileName = fileNames[nFile];
      (filesData + nFile)->finished = false;
      (filesData + nFile)->nWords = 0;
      (filesData + nFile)->nWordsBV = 0;
      (filesData + nFile)->nWordsEC = 0;
      (filesData + nFile)->previousCh = 32;
      /* get the file pointer */
      if (((filesData + nFile)->fp = fopen(fileNames[nFile], "rb")) == NULL)
      {
        printf("Error: could not open file %s\n", fileNames[nFile]);
        exit(EXIT_FAILURE);
      }

      /* while file is processing */
      while (!((filesData + nFile)->finished))
      {
        /* Send a chunk of data to each worker process for processing */
        for (nWorkers = 1; nWorkers < size; nWorkers++)
        {
          if ((filesData + nFile)->finished)
          {
            fclose((filesData + nFile)->fp); /* close the file pointer */
            break;
          }

          previousCh = (filesData + nFile)->previousCh;
          (filesData + nFile)->chunkSize = fread(chunk, 1, maxBytesPerChunk - 7, (filesData + nFile)->fp);
          
          /* if the chunk read is smaller than the value expected it means the current file has reached the end */
          if ((filesData + nFile)->chunkSize < (maxBytesPerChunk - 7)) 
            (filesData + nFile)->finished = true;
          else
            getChunkSizeAndLastChar(chunk, filesData + nFile);

          if ((filesData + nFile)->previousCh == EOF) /* checks the last character was the EOF */
            (filesData + nFile)->finished = true;

          /* send to the worker: */
          MPI_Send(&workStatus, 1, MPI_INT, nWorkers, 0, MPI_COMM_WORLD); /* a flag saying if there is work to do */
          MPI_Send(chunk, maxBytesPerChunk, MPI_UNSIGNED_CHAR, nWorkers, 0, MPI_COMM_WORLD);/* the chunk buffer */
          MPI_Send(&(filesData + nFile)->chunkSize, 1, MPI_INT, nWorkers, 0, MPI_COMM_WORLD);/* the size of the chunk */
          MPI_Send(&previousCh, 1, MPI_INT, nWorkers, 0, MPI_COMM_WORLD);/* the character of the previous chunk */

          memset(chunk, 0, maxBytesPerChunk * sizeof(unsigned char));
        }

        for (i = 1; i < nWorkers; i++)
        {
          /* Receive the processing results from each worker process */
          MPI_Recv(&nWords, 1, MPI_INT, i, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
          MPI_Recv(&nWordsBV, 1, MPI_INT, i, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
          MPI_Recv(&nWordsEC, 1, MPI_INT, i, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

          /* update struct with new results */
          (filesData + nFile)->nWords += nWords;
          (filesData + nFile)->nWordsBV += nWordsBV;
          (filesData + nFile)->nWordsEC += nWordsEC;
        }
      }
    }

    /* no more work to be done */
    workStatus = ALL_FILES_PROCESSED;
    /* inform workers that all files are process and they can exit */
    for (i = 1; i < size; i++)
      MPI_Send(&workStatus, 1, MPI_INT, i, 0, MPI_COMM_WORLD);

    /* timer ends */
    clock_gettime(CLOCK_MONOTONIC_RAW, &finish); /* end of measurement */

    /* print the results of the text processing */
    printResults(filesData, numFiles);

    /* calculate the elapsed time */
    printf("\nElapsed time = %.6f s\n", (finish.tv_sec - start.tv_sec) / 1.0 + (finish.tv_nsec - start.tv_nsec) / 1000000000.0);
  }
  else
  {
    MPI_Bcast(&maxBytesPerChunk, 1, MPI_INT, 0, MPI_COMM_WORLD);

    /* allocating memory for the file data structure */
    struct fileData *data = (struct fileData *)malloc(sizeof(struct fileData));
    data->nWords = 0;
    data->nWordsBV = 0;
    data->nWordsEC = 0;
    /* allocating memory for the chunk buffer */
    data->chunk = (unsigned char *)malloc(maxBytesPerChunk * sizeof(unsigned char));

    while (true)
    {
      MPI_Recv(&workStatus, 1, MPI_UNSIGNED, 0, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
      if (workStatus == ALL_FILES_PROCESSED)
        break;

      MPI_Recv(data->chunk, maxBytesPerChunk, MPI_UNSIGNED_CHAR, 0, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
      MPI_Recv(&data->chunkSize, 1, MPI_INT, 0, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
      MPI_Recv(&data->previousCh, 1, MPI_INT, 0, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

      /* perform text processing on the chunk */
      processChunk(data);
      /* Send the processing results to the dispatcher */
      MPI_Send(&data->nWords, 1, MPI_INT, 0, 0, MPI_COMM_WORLD);
      MPI_Send(&data->nWordsBV, 1, MPI_INT, 0, 0, MPI_COMM_WORLD);
      MPI_Send(&data->nWordsEC, 1, MPI_INT, 0, 0, MPI_COMM_WORLD);
      /* reset structures */
      data->nWords = 0;
      data->nWordsBV = 0;
      data->nWordsEC = 0;
    }
  }

  MPI_Finalize();
  exit(EXIT_SUCCESS);
}

/**
 *  \brief Print command usage.
 *
 *  A message specifying how the program should be called is printed.
 *
 *  \param cmdName string with the name of the command
 */
static void printUsage(char *cmdName)
{
  fprintf(stderr, "\nSynopsis: %s OPTIONS [filename / maximum number of bytes per chunk]\n"
                  "  OPTIONS:\n"
                  "  -h      --- print this help\n"
                  "  -f      --- filename to process\n"
                  "  -m      --- maximum number of bytes per chunk\n",
          cmdName);
}

/**
 *  \brief Print results of the text processing.
 *
 *  Operation carried out by the dispatcher process.
 */
void printResults(struct fileData *filesData, int numFiles)
{
  for (int i = 0; i < numFiles; i++)
  {
    printf("\nFile name: %s\n", (filesData + i)->fileName);
    printf("Total number of words = %d\n", (filesData + i)->nWords);
    printf("N. of words beginning with a vowel = %d\n", (filesData + i)->nWordsBV);
    printf("N. of words ending with a consonant = %d\n", (filesData + i)->nWordsEC);
  }
}