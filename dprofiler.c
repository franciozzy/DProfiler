/*
 * --------------------------
 *  Disk Throughput Profiler
 * --------------------------
 *  dprofiler.c
 * -------------
 *  Copyright 2011-2012 (c) Felipe Franciosi <felipe@paradoxo.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Read the README file for the changelog and information on how to
 * compile and use this program.
 */

// Global definitions (don't mess with those)
#define	_GNU_SOURCE
#define	_FILE_OFFSET_BITS	64
#define	IO_OP_READ		1
#define	IO_OP_WRITE		2
#define MT_PROGNAME		"Disk Throughput Profiler"
#ifdef	CLOCK_MONOTONIC_RAW
#define	MT_CLOCK		CLOCK_MONOTONIC_RAW
#else
#define	MT_CLOCK		CLOCK_MONOTONIC
#endif

// Header files
#include <stdio.h>			// printf, perror, snprintf, fopen, fputs, fclose
#include <stdlib.h>			// posix_memalign, free
#include <string.h>			// memset
#include <inttypes.h>			// PRI*
#include <sys/types.h>			// open, lseek64, getpid
#include <sys/stat.h>			// open
#include <fcntl.h>			// open
#include <unistd.h>			// close, lseek64, getpid, getpagesize, read
#include <time.h>			// clock_gettime

// Global default definitions
#define	MT_DATAFILE	"dprofiler"	// Default File prefix for data output
#define	MT_BUFSIZE	1024*1024	// Default 1MB i/o operations
#define	MT_WZERO	1		// Default write behaviour
#define	MT_OSYNC	0		// Default usage of O_SYNC flag upon open()

// Auxiliary functions
void usage(char *argv0){
	// Local variables
	int		i;		// Temporary integer

	// Print help
	for (i=0; i<strlen(MT_PROGNAME); i++) fprintf(stderr, "-");
	fprintf(stderr, "\n%s\n", MT_PROGNAME);
	for (i=0; i<strlen(MT_PROGNAME); i++) fprintf(stderr, "-");
	fprintf(stderr, "\nUsage: %s < -r | -w > < -d dev_name > [ -h ] [ -v[v] ] [ -b <buf_size> ] [ -g <group_size> ] [ -o <datafile> ]\n", argv0);
	fprintf(stderr, "       -r | -w         Read from or write to device.\n");
	fprintf(stderr, "       -d dev_name     Specify block device to operate on.\n");
	fprintf(stderr, "                       !!WARNING!! Be careful when using -w, as the device will be overwritten.\n");
	fprintf(stderr, "       -h              Print this help message and quit.\n");
//	fprintf(stderr, "       -z              When writing, write only zeros instead of random data (which is default).\n");
	fprintf(stderr, "       -v              Increase verbose level (may be used multiple times).\n");
	fprintf(stderr, "       -b buf_size     Specify different buffer size (in bytes, default=%d).\n", MT_BUFSIZE);
	fprintf(stderr, "       -g group_size   Group measurements together and average results accordingly (default=1).\n");
	fprintf(stderr, "       -o datafile     Specify output datafile name (default=%s<pid>.dat).\n", MT_DATAFILE);
	fprintf(stderr, "       -y              Open device with O_SYNC (see open(2) man page).\n");
}


// Main function
int main(int argc, char **argv){
	// Local variables

	// IO related
	char		*buf = NULL;	// IO Buffer
	off_t		bufsize = 0;	// IO Buffer size
	char		io_op = -1;	// IO Operation (IO_OP_READ or IO_OP_WRITE)
	char		wzero = -1;	// Write zeros instead of random data
	char		osync = -1;	// Open device with O_SYNC

	// Datafile related
	FILE		*datafp = NULL;	// Data file file pointer
	char		*datafn = NULL;	// Data file file name

	// Block device related
	int		bdevfd = -1;	// Block device file descriptor
	char		*bdevfn = NULL;	// Block device file name
	int		bdflags;	// Block device open flags
	off_t		bdevsize;	// Block device size

	// System related
	int		psize;		// System pagesize

	// Summary related
	ssize_t		bytesioed;	// Number of bytes io'ed (each io)
	u_int64_t	count = 0;	// Number of bytes io'ed (accumulated)
	u_int32_t	gsize = 1;	// Group size for outputs
	u_int32_t	gcount;		// Group counter
	u_int64_t	gstart;		// Group start address
	u_int64_t	timeacc;	// Time accumulator for grouping
	struct timespec	st1;		// Start time
	struct timespec	st2;		// Finish time

	// General
	int		err = 0;	// Return error code
	int		verbose = 0;	// Verbose level
	int		i,j;		// Temporary integers

	
	// Fetch arguments
	while ((i = getopt(argc, argv, "hvd:b:g:o:rwzy")) != -1){
		switch (i){
			case 'h':
				// Print help
				usage(argv[0]);
				goto out;

			case 'r':
				// Set io_op to read, if unset
				if (io_op != -1){
					fprintf(stderr, "%s: Invalid argument \"-r\", operation mode already set.\n", argv[0]);
					err = 1;
					goto out;
				}
				io_op = IO_OP_READ;
				break;

			case 'w':
				// Set io_op to write, if unset
				if (io_op != -1){
					fprintf(stderr, "%s: Invalid argument \"-w\", operation mode already set.\n", argv[0]);
					err = 1;
					goto out;
				}
				io_op = IO_OP_WRITE;
				break;

			case 'd':
				// Set a block device
				if (bdevfn){
					fprintf(stderr, "%s: Invalid argument \"-d\", device already set.\n", argv[0]);
					err = 1;
					goto out;
				}
				if ((bdevfn = (char *)malloc(strlen(optarg)+1)) == NULL){
					perror("malloc");
					fprintf(stderr, "%s: Error allocating memory for block device name.\n", argv[0]);
					err = 1;
					goto out;
				}
				sprintf(bdevfn, "%s", optarg);
				break;

			case 'z':
				// Set write to zeros
				if (wzero == 1){
					fprintf(stderr, "%s: Invalid argument \"-z\", already set to write zeros.\n", argv[0]);
					err = 1;
					goto out;
				}
				wzero = 1;
				break;

			case 'v':
				// Increase verbose level
				verbose++;
				break;


			case 'b':
				// Set a different buffer size
				if (bufsize > 0){
					fprintf(stderr, "%s: Invalid argument \"-b\", buffer already set.\n", argv[0]);
					err = 1;
					goto out;
				}
				bufsize = atoi(optarg);
				if (bufsize == 0){
					fprintf(stderr, "%s: Invalid value for \"-b\". Buffer must be greater than zero.\n", argv[0]);
					err = 1;
					goto out;
				}
				break;

			case 'g':
				// Set group size
				gsize = atoi(optarg);
				if (gsize == 0){
					gsize = 1;
				}
				break;

			case 'o':
				// Set datafile name
				if (datafn != NULL){
					fprintf(stderr, "%s: Invalid argument \"-o\", data file already specified.\n", argv[0]);
					err = 1;
					goto out;
				}
				if ((datafn = (char *)malloc(strlen(optarg)+1)) == NULL){
					perror("malloc");
					fprintf(stderr, "Error reallocating memory for data file name.\n");
					err = 1;
					goto out;
				}
				sprintf(datafn, "%s", optarg);
				break;

			case 'y':
				// Set usage of O_SYNC
				if (osync != -1){
					fprintf(stderr, "%s: Invalid argument \"-y\", usage of O_SYNC already set.\n", argv[0]);
					err = 1;
					goto out;
				}
				osync = 1;
				break;
		}
	}

	// Verify if operation mode was set
	if ((io_op != IO_OP_READ) && (io_op != IO_OP_WRITE)){
		fprintf(stderr, "%s: Error, you must specify \"-r\" or \"-w\".\n\n", argv[0]);
		usage(argv[0]);
		err = 1;
		goto out;
	}

	// If wzero has been turned on, check if writing
	if ((wzero != 0) && (io_op != IO_OP_WRITE)){
		fprintf(stderr, "%s: Error, \"-z\" can only be used with \"-w\".\n\n", argv[0]);
		usage(argv[0]);
		err = 1;
		goto out;
	}

	// Verify if a block device has been specified
	if (bdevfn == NULL){
		fprintf(stderr, "%s: Error, you must specify a block device with \"-d\"\n\n", argv[0]);
		usage(argv[0]);
		err = 1;
		goto out;
	}

	// Set datafn to default if it hasn't been specified
	if (datafn == NULL){
		if ((datafn = (char *)malloc(strlen(MT_DATAFILE)+20)) == NULL){	// 20 = ceiling(len of pid_t + ".dat" + '\0')
			perror("malloc");
			fprintf(stderr, "%s: Error allocating memory for data file name.\n", argv[0]);
			err = 1;
			goto out;
		}
		snprintf(datafn, strlen(MT_DATAFILE)+20, "%s%d.dat", MT_DATAFILE, getpid());
	}

	// Set buffer size to default if it hasn't been specified
	if (bufsize == 0){
		bufsize = MT_BUFSIZE;
	}

	// Set SYNC flag to default if it hasn't been specified
	if (osync == -1){
		osync = MT_OSYNC;
	}

	// Open block device
	bdflags = O_RDWR | O_DIRECT | O_LARGEFILE;
	if (osync){
		bdflags |= O_SYNC;
	}
	if ((bdevfd = open(bdevfn, bdflags, 0)) < 0){
		perror("open");
		fprintf(stderr, "%s: Error opening block device \"%s\".\n", argv[0], bdevfn);
		err = 1;
		goto out;
	}

	// Move the pointer to the end of the block device (fetchs block device size)
	if ((bdevsize = lseek(bdevfd, 0, SEEK_END)) < 0){
		perror("lseek");
		fprintf(stderr, "%s: Error repositioning offset to eof.\n", argv[0]);
		err = 1;
		goto out;
	}

	// Move the pointer back to the beginning of the block device
	if (lseek(bdevfd, 0, SEEK_SET) < 0){
		perror("lseek");
		fprintf(stderr, "%s: Error repositioning offset to start.\n", argv[0]);
		err = 1;
		goto out;
	}

	// Fetch system pagesize
	psize = getpagesize();

	// Allocates rbuf according to the systems page size
	if (posix_memalign((void **)&(buf), psize, bufsize) != 0){
		//perror("posix_memalign"); // posix_memalign doesn't set errno.
		fprintf(stderr, "%s: Error malloc'ing aligned buf, %llu bytes long.\n", argv[0], bufsize);
		err = 1;
		goto out;
	}

	// Open datafile
	if ((datafp = fopen(datafn, "w")) == NULL){
		perror("fopen");
		fprintf(stderr, "%s: Error opening datafile \"%s\" for writting.\n", argv[0], datafn);
		err = 1;
		goto out;
	}

	// Print verbose stuff
	if (verbose){
		for (j=0; j<strlen(MT_PROGNAME); j++) fprintf(stderr, "-");
		fprintf(stderr, "\n%s\n", MT_PROGNAME);
		for (j=0; j<strlen(MT_PROGNAME); j++) fprintf(stderr, "-");
		fprintf(stderr, "\nDevice \"%s\" has %llu bytes\n", bdevfn, bdevsize);
		fprintf(stderr, "System pagesize is %d bytes long.\n", psize);
		fprintf(stderr, "Got aligned buffer at %p.\n", buf);
		fprintf(stderr, "Buffer is %llu bytes long.\n", bufsize);
		fprintf(stderr, "Grouped at every %d outputs.\n", gsize);
		fprintf(stderr, "Datafile is \"%s\".\n", datafn);
		fprintf(stderr, "------------------------------------------\n");
		fflush(stderr);
	}

	// Initialize loop
	bytesioed = 1;
	gcount = gsize;
	gstart = 0;
	timeacc = 0;

	// IO device until its end
	while(bytesioed > 0){
		// Prepare buffer
		switch(io_op){
			case IO_OP_WRITE:
				// Decide what to write
				if (wzero){
					// Use only zeros
					memset(buf, 0, bufsize);
				}else{
					// Use random data
					memset(buf, 0, bufsize);
					// FIXME: I'm just doing zeros here for now.
				}
				break;
			case IO_OP_READ:
				// Reset read buffer
				memset(buf, 0, bufsize);
				break;
			default:
				// Invalid IO operation
				fprintf(stderr, "%s: Error, unsupported IO operation (code %d).\n", argv[0], io_op);
				free(buf);
				close(bdevfd);
				fclose(datafp);
				return(1);
		}

		// Perform IO op
		if (io_op == IO_OP_WRITE){
			// Start the clock
			clock_gettime(MT_CLOCK, &st1);

			// Write
			bytesioed = write(bdevfd, buf, bufsize);

			// Stop the clock
			clock_gettime(MT_CLOCK, &st2);

		}else
		/* if (io_op == IO_OP_READ) // needed if more IO_codes are supported in the future */
		{
			// Start the clock
			clock_gettime(MT_CLOCK, &st1);

			// Read
			bytesioed = read(bdevfd, buf, bufsize);

			// Stop the clock
			clock_gettime(MT_CLOCK, &st2);
		}

		// Increase the counter
		count += bytesioed;

		// Accumulate time difference
		timeacc += (((long long unsigned)st2.tv_sec*1000000)+((long long unsigned)st2.tv_nsec/1000))-
                           (((long long unsigned)st1.tv_sec*1000000)+((long long unsigned)st1.tv_nsec/1000));

		// Print it whenever group is complete
		if (--gcount == 0){
			// Write to data file
			sprintf(buf, "%013"PRIu64" %"PRIu64"\n", gstart, timeacc);
			fputs(buf, datafp);
			fflush(datafp);

			// If verbose level is 2, also print on stdout
			if (verbose > 1)
			{
				fprintf(stdout, "%013llu %llu\n", gstart, timeacc);
				fflush(stdout);
			}

			// Reset group counter
			gcount = gsize;

			// Reset group start address
			gstart = count;

			// Reset time accumulator
			timeacc = 0;
		}
	}

out:
	// Free various buffers
	if (buf){
		// I/O Buffer
		free(buf);
	}
	if (bdevfn){
		// Block device file name
		free(bdevfn);
	}
	if (datafn){
		// Data file name
		free(datafn);
	}

	// Close data file pointer
	if (datafp){
		fclose(datafp);
	}

	// Close block device descriptor
	if (bdevfd != -1){
		close(bdevfd);
	}

	// Return
	return(err);
}
