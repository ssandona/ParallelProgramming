/*
    N-Body simulation code.
*/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>
#include <mpi.h>

extern double   sqrt(double);
extern double   atan2(double, double);

#define GRAVITY     1.1
#define FRICTION    0.01
#define MAXBODIES  10000
#define DELTA_T     (0.025/5000)
#define BOUNCE      -0.9
#define SEED        27102015

typedef struct {
    double x[2];        /* Old and new X-axis coordinates */
    double y[2];        /* Old and new Y-axis coordinates */
    // double xf;          /* force along X-axis */
    // double yf;          /* force along Y-axis */
    double xv;          /* velocity along X-axis */
    double yv;          /* velocity along Y-axis */
    double mass;        /* Mass of the body */
    double radius;      /* width (derived from mass) */
} bodyType;

typedef struct {
    double xf;          /* force along X-axis */
    double yf;          /* force along Y-axis */
} forceType;


bodyType bodies[MAXBODIES];
forceType *forces;
forceType *new_forces;
forceType *new_forces2;
int bodyCt;
int old = 0;    /* Flips between 0 and 1 */
bodyType *new_bodies;
bodyType *new_bodies2;
bodyType *rec_bodies;
int *displs;
int *bodies_per_proc;
int myid;
int printed = 0;
int numprocs;
MPI_Op mpi_sum;


/*  Macros to hide memory layout
*/
#define X(B)        bodies[B].x[old]
#define XN(B)       bodies[B].x[old^1]
#define Y(B)        bodies[B].y[old]
#define YN(B)       bodies[B].y[old^1]
#define XF(B)       forces[B].xf
#define YF(B)       forces[B].yf
#define XV(B)       bodies[B].xv
#define YV(B)       bodies[B].yv
#define R(B)        bodies[B].radius
#define M(B)        bodies[B].mass

/*  Macros to hide memory layout
*/
#define _X(B)       new_bodies[B].x[old]
#define _XN(B)      new_bodies[B].x[old^1]
#define _Y(B)       new_bodies[B].y[old]
#define _YN(B)      new_bodies[B].y[old^1]
#define _XF(B)      new_forces[B].xf
#define _YF(B)      new_forces[B].yf
#define _XV(B)      new_bodies[B].xv
#define _YV(B)      new_bodies[B].yv
#define _R(B)       new_bodies[B].radius
#define _M(B)       new_bodies[B].mass

/*  Dimensions of space (very finite, ain't it?)
*/
int     xdim = 0;
int     ydim = 0;



/*  Graphic output stuff...
*/

#include <fcntl.h>
#include <sys/mman.h>

int     fsize;
unsigned char   *map;
unsigned char   *image;


unsigned char *
map_P6(char *filename,
       int *xdim,
       int *ydim) {
    /* The following is a fast and sloppy way to
       map a color raw PPM (P6) image file
    */
    int fd;
    unsigned char *p;
    int maxval;

    /* First, open the file... */
    if ((fd = open(filename, O_RDWR)) < 0) {
        return((unsigned char *) 0);
    }

    /* Read size and map the whole file... */
    fsize = lseek(fd, ((off_t) 0), SEEK_END);
    map = ((unsigned char *)
           mmap(0,      /* Put it anywhere */
                fsize,  /* Map the whole file */
                (PROT_READ | PROT_WRITE),   /* Read/write */
                MAP_SHARED, /* Not just for me */
                fd,     /* The file */
                0));    /* Right from the start */
    if (map == ((unsigned char *) - 1)) {
        close(fd);
        return((unsigned char *) 0);
    }

    /* File should now be mapped; read magic value */
    p = map;
    if (*(p++) != 'P') goto ppm_exit;
    switch (*(p++)) {
    case '6':
        break;
    default:
        goto ppm_exit;
    }

#define Eat_Space \
    while ((*p == ' ') || \
           (*p == '\t') || \
           (*p == '\n') || \
           (*p == '\r') || \
           (*p == '#')) { \
        if (*p == '#') while (*(++p) != '\n') ; \
        ++p; \
    }

    Eat_Space;      /* Eat white space and comments */

#define Get_Number(n) \
    { \
        int charval = *p; \
 \
        if ((charval < '0') || (charval > '9')) goto ppm_exit; \
 \
        n = (charval - '0'); \
        charval = *(++p); \
        while ((charval >= '0') && (charval <= '9')) { \
            n *= 10; \
            n += (charval - '0'); \
            charval = *(++p); \
        } \
    }

    Get_Number(*xdim);  /* Get image width */

    Eat_Space;      /* Eat white space and comments */
    Get_Number(*ydim);  /* Get image width */

    Eat_Space;      /* Eat white space and comments */
    Get_Number(maxval); /* Get image max value */

    /* Should be 8-bit binary after one whitespace char... */
    if (maxval > 255) {
ppm_exit:
        close(fd);
        munmap(map, fsize);
        return((unsigned char *) 0);
    }
    if ((*p != ' ') &&
            (*p != '\t') &&
            (*p != '\n') &&
            (*p != '\r')) goto ppm_exit;

    /* Here we are... next byte begins the 24-bit data */
    return(p + 1);

    /* Notice that we never clean-up after this:

       close(fd);
       munmap(map, fsize);

       However, this is relatively harmless;
       they will go away when this process dies.
    */
}

#undef  Eat_Space
#undef  Get_Number

static inline void
color(int x, int y, int b) {
    unsigned char *p = image + (3 * (x + (y * xdim)));
    int tint = ((0xfff * (b + 1)) / (bodyCt + 2));

    p[0] = (tint & 0xf) << 4;
    p[1] = (tint & 0xf0);
    p[2] = (tint & 0xf00) >> 4;
}

static inline void
black(int x, int y) {
    unsigned char *p = image + (3 * (x + (y * xdim)));

    p[2] = (p[1] = (p[0] = 0));
}

void
display(void) {
    double i, j;
    int b;

    /* For each pixel */
    for (j = 0; j < ydim; ++j) {
        for (i = 0; i < xdim; ++i) {
            /* Find the first body covering here */
            for (b = 0; b < bodyCt; ++b) {
                double dy = Y(b) - j;
                double dx = X(b) - i;
                double d = sqrt(dx * dx + dy * dy);

                if (d <= R(b) + 0.5) {
                    /* This is it */
                    color(i, j, b);
                    goto colored;
                }
            }

            /* No object -- empty space */
            black(i, j);

colored:
            ;
        }
    }
}

void
print(void) {
    int b;
    for (b = 0; b < bodyCt; ++b) {
        printf("%10.3f %10.3f %10.3f %10.3f %10.3f %10.3f\n", _X(b), _Y(b), _XF(b), _YF(b), _XV(b), _YV(b));
    }
}



void
clear_forces(void) {
    int b;

    /* Clear force accumulation variables */
    for (b = 0; b < bodyCt; ++b) {
        _YF(b) = (_XF(b) = 0);
    }
}

void compute(int b, int c) {
    double dx = _X(c) - _X(b);
    double dy = _Y(c) - _Y(b);
    double angle = atan2(dy, dx);
    double dsqr = dx * dx + dy * dy;
    double mindist = _R(b) + _R(c);
    double mindsqr = mindist * mindist;
    double forced = ((dsqr < mindsqr) ? mindsqr : dsqr);
    double force = _M(b) * _M(c) * GRAVITY / forced;
    double xf = force * cos(angle);
    double yf = force * sin(angle);

    /*if(printed <= 1 && myid == 1) {
        printf("body: %d, from: %d, INCREMENT FORCE (BEFORE): (XF:%10.3f,YF:%10.3f)\n", b, c, _XF(b), _YF(b));
         printf("val-> dx:%10.3f, dy:%10.3f, angle:%10.3f, dsqr:%10.3f, mindist:%10.3f\n", dx, dy, angle, dsqr, mindist);
         printf("mindsqr:%10.3f, forced:%10.3f, force:%10.3f, xf:%10.3f, yf:%10.3f\n", mindsqr, forced, force, xf, yf);

    }*/

    _XF(b) += xf;
    _YF(b) += yf;
    _XF(c) -= xf;
    _YF(c) -= yf;
}

void
compute_forces(void) {
    int b, c, i;
    for(i = 0; i < numprocs; i++) {
        /*if(myid == 1)
            printf("__I__:%d\n", i);*/
        if(myid > i) {
            /*if(myid == 1)
                printf("myid > i\n");*/
            for(b = (displs[i] + displs[i] + bodies_per_proc[i]) / 2; b < displs[i] + bodies_per_proc[i]; b++) {
                for(c = displs[myid]; c < displs[myid] + bodies_per_proc[myid]; c++) {
                    /*if(myid == 1)
                        printf("b:%d, c:%d\n", b, c);*/
                    compute(b, c);
                }
            }
        } else {
            if(myid < i) {
                /*if(myid == 1){
                    printf("myid < i\n");
                    printf("for(b=%d; b<%d;b++)",displs[myid],(displs[myid] + displs[myid] + bodies_per_proc[myid]) / 2);
                    printf("for(b=%d; b<%d;b++)\n",displs[i], displs[i] + bodies_per_proc[i]);
                }*/
                for(b = displs[myid]; b < (displs[myid] + displs[myid] + bodies_per_proc[myid]) / 2; b++) {
                    for(c = displs[i]; c < displs[i] + bodies_per_proc[i]; c++) {
                        /*if(myid == 1)
                            printf("b:%d, c:%d\n", b, c);*/
                        compute(b, c);
                    }
                }
            } else {
                /*if(myid == 1)
                    printf("myid = i\n");*/
                for(b = displs[myid]; b < displs[myid] + bodies_per_proc[myid]; b++) {
                    for(c = b + 1; c < displs[myid] + bodies_per_proc[myid]; c++) {
                        /*if(myid == 1)
                            printf("b:%d, c:%d\n", b, c);*/
                        compute(b, c);
                    }
                }
            }
        }

    }
}


/*



int cont = 0;
//printf("id %d: (%d,%d)\n",myid, displs[myid],displs[myid] + bodies_per_proc[myid]);
for (b = displs[myid]; b < displs[myid] + bodies_per_proc[myid]; ++b) {
    for (c = 0; c < bodyCt; ++c) {
        if(c == b)
            continue;
        if(c >= displs[myid] && c < displs[myid] + bodies_per_proc[myid]) {
            if(c < b) {
                continue;
            }
        }
        //printf("id %d: c:%d, b:%d -> \n",myid,c,b);
        double dx = _X(c) - _X(b);
        double dy = _Y(c) - _Y(b);
        double angle = atan2(dy, dx);
        double dsqr = dx * dx + dy * dy;
        double mindist = _R(b) + _R(c);
        double mindsqr = mindist * mindist;
        double forced = ((dsqr < mindsqr) ? mindsqr : dsqr);
        double force = _M(b) * _M(c) * GRAVITY / forced;
        double xf = force * cos(angle);
        double yf = force * sin(angle);

        _XF(b) += xf;
        _YF(b) += yf;


        _XF(c) -= xf;
        _YF(c) -= yf;
    }
}
}
*/

void
compute_velocities(void) {
    int b;

    for (b = 0; b < bodyCt; ++b) {
        //for (b = displs[myid]; b < displs[myid] + bodies_per_proc[myid]; ++b) {
        double xv = _XV(b);
        double yv = _YV(b);
        double force = sqrt(xv * xv + yv * yv) * FRICTION;
        double angle = atan2(yv, xv);
        double xf = _XF(b) - (force * cos(angle));
        double yf = _YF(b) - (force * sin(angle));

        _XV(b) += (xf / _M(b)) * DELTA_T;
        _YV(b) += (yf / _M(b)) * DELTA_T;
    }
}

void
compute_positions(void) {
    int b;
    for (b = 0; b < bodyCt; ++b) {
        //for (b = displs[myid]; b < displs[myid] + bodies_per_proc[myid]; ++b) {
        double xn = _X(b) + (_XV(b) * DELTA_T);
        double yn = _Y(b) + (_YV(b) * DELTA_T);

        /* Bounce of image "walls" */
        if (xn < 0) {
            xn = 0;
            _XV(b) = -_XV(b);
        } else if (xn >= xdim) {
            xn = xdim - 1;
            _XV(b) = -_XV(b);
        }
        if (yn < 0) {
            yn = 0;
            _YV(b) = -_YV(b);
        } else if (yn >= ydim) {
            yn = ydim - 1;
            _YV(b) = -_YV(b);
        }

        /* Update position */
        _XN(b) = xn;
        _YN(b) = yn;
    }
}


void
print_mine(void) {
    int b;
    printf("my chunk =>(%d,%d)\n", displs[myid], displs[myid] + bodies_per_proc[myid] );
    for (b = displs[myid]; b < displs[myid] + bodies_per_proc[myid]; ++b) {
        printf("%10.3f %10.3f %10.3f %10.3f %10.3f %10.3f\n", _X(b), _Y(b), _XF(b), _YF(b), _XV(b), _YV(b));
    }
}

void sumForces(forceType *in, forceType *inout, int *len, MPI_Datatype *dtype) {
    int i;
    for (i = 0; i < *len; ++i) {
        inout->xf += in->xf;
        inout->yf += in->yf;
        in++;
        inout++;
    }
}




/*  Main program...
*/

int
main(int argc, char **argv) {
    unsigned int lastup = 0;
    unsigned int secsup;
    int b;
    int steps;
    double rtime;
    struct timeval start;
    struct timeval end;
    int i;
    int  namelen;
    char processor_name[MPI_MAX_PROCESSOR_NAME];

    /* MPI_Init(&argc, &argv);
     MPI_Comm_size(MPI_COMM_WORLD, &numprocs);
     MPI_Comm_rank(MPI_COMM_WORLD, &myid);
     MPI_Get_processor_name(processor_name, &namelen);

     fprintf(stderr, "Process %d on %s\n", myid, processor_name);*/

    if (argc != 5) {
        fprintf(stderr,
                "Usage: %s num_bodies secs_per_update ppm_output_file steps\n",
                argv[0]);
        exit(1);
    }
    /*fprintf(stderr, "0 => %s\n", argv[0]);
    fprintf(stderr, "1 => %s\n", argv[1]);
    fprintf(stderr, "2 => %s\n", argv[2]);
    fprintf(stderr, "3 => %s\n", argv[3]);
    fprintf(stderr, "4 => %s\n", argv[4]);*/
    if ((bodyCt = atol(argv[1])) > MAXBODIES ) {
        fprintf(stderr, "Using only %d bodies...\n", MAXBODIES);
        bodyCt = MAXBODIES;
    } else if (bodyCt < 2) {
        fprintf(stderr, "Using two bodies...\n");
        bodyCt = 2;
    }

    forces = malloc(sizeof(forceType) * bodyCt);
    for(i = 0; i < bodyCt; i++) {
        forces[i].xf = 0;
        forces[i].yf = 0;
    }
    /*if(bodyCt > numprocs) {
        bodyCt = numprocs;
    }*/
    new_bodies = malloc(sizeof(bodyType) * bodyCt);

    secsup = atoi(argv[2]);
    image = map_P6(argv[3], &xdim, &ydim);
    steps = atoi(argv[4]);

    fprintf(stderr, "Running N-body with %i bodies and %i steps\n", bodyCt, steps);

    /* Initialize simulation data */
    srand(SEED);
    for (b = 0; b < bodyCt; ++b) {
        _X(b) = (rand() % xdim);
        _Y(b) = (rand() % ydim);
        _R(b) = 1 + ((b * b + 1.0) * sqrt(1.0 * ((xdim * xdim) + (ydim * ydim)))) /
               (25.0 * (bodyCt * bodyCt + 1.0));
        _M(b) = _R(b) * _R(b) * _R(b);
        _XV(b) = ((rand() % 20000) - 10000) / 2000.0;
        _YV(b) = ((rand() % 20000) - 10000) / 2000.0;
    }
    //fprintf(stderr, "a\n");


    MPI_Init(&argc, &argv);
    MPI_Comm_size(MPI_COMM_WORLD, &numprocs);
    MPI_Comm_rank(MPI_COMM_WORLD, &myid);
    MPI_Get_processor_name(processor_name, &namelen);

    fprintf(stderr, "Process %d on %s\n", myid, processor_name);




    bodies_per_proc = malloc(sizeof(int) * numprocs);
    displs = malloc(sizeof(int) * numprocs);
    int rem = bodyCt % numprocs; // elements remaining after division among processes
    //fprintf(stderr, "rem => %d\n", rem);

    //fprintf(stderr, "b\n");
    /* create a type for struct bodyType */
    const int nitems = 2;
    int blocklengths[2] = {1, 1};
    MPI_Datatype types[2] = {MPI_DOUBLE, MPI_DOUBLE};
    MPI_Datatype mpi_force_type;
    MPI_Aint     offsets[2];

    offsets[0] = offsetof(forceType, xf);
    offsets[1] = offsetof(forceType, yf);

    MPI_Type_create_struct(nitems, blocklengths, offsets, types, &mpi_force_type);
    MPI_Type_commit(&mpi_force_type);


    const int nitems2 = 6;
    int blocklengths2[6] = {2, 2, 1, 1, 1, 1};
    MPI_Datatype types2[6] = {MPI_DOUBLE, MPI_DOUBLE, MPI_DOUBLE, MPI_DOUBLE, MPI_DOUBLE, MPI_DOUBLE,};
    MPI_Datatype mpi_body_type;
    MPI_Aint     offsets2[6];

    offsets2[0] = offsetof(bodyType, x);
    offsets2[1] = offsetof(bodyType, y);
    offsets2[2] = offsetof(bodyType, xv);
    offsets2[3] = offsetof(bodyType, yv);
    offsets2[4] = offsetof(bodyType, mass);
    offsets2[5] = offsetof(bodyType, radius);

    MPI_Type_create_struct(nitems2, blocklengths2, offsets2, types2, &mpi_body_type);
    MPI_Type_commit(&mpi_body_type);




    //fprintf(stderr, "c\n");

    MPI_Op_create((MPI_User_function *) sumForces, 1, &mpi_sum);

    int avarage_bodies_per_proc = bodyCt / numprocs;
    //fprintf(stderr, "avarage => %d\n", avarage_bodies_per_proc);
    int sum = 0;

    //fprintf(stderr, "d\n");
    // calculate send counts and displacements
    for (i = 0; i < numprocs; i++) {
        bodies_per_proc[i] = avarage_bodies_per_proc;
        if (rem > 0) {
            bodies_per_proc[i]++;
            rem--;
        }
        displs[i] = sum;
        sum += bodies_per_proc[i];
    }

    /*printf("displs and bodies\n");
    for(i = 0; i < numprocs; i++) {
        printf("i:%d, displs:%d, numBodies:%d\n", i, displs[i], bodies_per_proc[i]);
    }*/
    //fprintf(stderr, "e\n");

    //printf("bodies_per_proc[%d] = %d\tdispls[%d] = %d\n", 0, bodies_per_proc[0], 0, displs[0]);

    int bufSize = bodyCt % numprocs == 0 ? bodyCt / numprocs : (bodyCt / numprocs + 1);
    // Create a buffer that will hold a subset of the bodies
    //fprintf(stderr, "bufsize: %d\n", bufSize);


    //rec_bodies = malloc(sizeof(bodyType) * bufSize);



    //fprintf(stderr, "f\n");

    // Scatter the bodies to all processes
    /*MPI_Scatter(bodies, bodies_per_proc, mpi_body_type, sub_bodies,
                bodies_per_proc, mpi_body_type, 0, MPI_COMM_WORLD);

    MPI_Scatterv(bodies, bodies_per_proc, bodyCt, sub_bodies,
                 avarage_bodies_per_proc, mpi_body_type, 0, MPI_COMM_WORLD);*/


    // print calculated send counts and displacements for each process
    /*if (0 == myid) {
        for (i = 0; i < numprocs; i++) {
            printf("bodies_per_proc[%d] = %d\tdispls[%d] = %d\n", i, bodies_per_proc[i], i, displs[i]);
        }
    }*/
    //fprintf(stderr, "g\n");

    // divide the data among processes as described by bodies_per_proc and displs
    //MPI_Scatterv(bodies, bodies_per_proc, displs, mpi_body_type, rec_bodies, bufSize, mpi_body_type, 0, MPI_COMM_WORLD);


    /////MPI_Scatterv(bodies, bodies_per_proc, displs, mpi_force_type, rec_bodies, bufSize, mpi_force_type, 0, MPI_COMM_WORLD);
    MPI_Bcast(new_bodies, bodyCt, mpi_body_type, 0, MPI_COMM_WORLD);

    //fprintf(stderr, "h\n");

    // print what each process received
    /*printf("__ID__: %d: ", myid);
    for (i = 0; i < bodies_per_proc[myid]; i++) {
        printf("\nbody: %d, mass: %d, pos: (%d,%d)", i, (int)(rec_bodies[i].mass), (int)rec_bodies[i].x[old], (int)rec_bodies[i].y[old]);
    }*/
    //fprintf(stderr, "i\n");
    //printf("\n");

    // new_bodies = malloc(sizeof(bodyType) * bodyCt);

    //MPI_Allgatherv(rec_bodies, bodies_per_proc[myid],mpi_body_type, new_bodies, bodies_per_proc, displs, mpi_body_type, MPI_COMM_WORLD);

    /* printf("__ID__: %d: ", myid);
     printf("__DIM__: %d: ", bodyCt);
     for (i = 0; i < bodyCt; i++) {
         printf("\nbody: %d, mass: %d, pos: (%d,%d)", i, (int)(new_bodies[i].mass), (int)new_bodies[i].x[old], (int)new_bodies[i].y[old]);
     }*/

    //fprintf(stderr, "k\n");

    int cont;

    new_forces = malloc(sizeof(forceType) * bodyCt);
    for(i = 0; i < bodyCt; i++) {
        new_forces[i].xf = 0;
        new_forces[i].yf = 0;
    }

    if(gettimeofday(&start, 0) != 0) {
        fprintf(stderr, "could not do timing\n");
        exit(1);
    }
    //printf("a\n");
    //new_bodies = malloc(sizeof(bodyType) * bodyCt);

    //MPI_Allgatherv(rec_bodies, bodies_per_proc[myid], mpi_body_type, new_bodies, bodies_per_proc, displs, mpi_body_type, MPI_COMM_WORLD);
    // printf("b\n");

    /* Main Loop */

    while (steps--) {
        cont = 0;
        clear_forces();
        /*if(printed <= 1 && myid == 0) {
            printf("A -> %d\n", myid);
            for (i = 0; i < bodyCt; i++) {
                printf("\nbody: %d, mass: %d, pos: (%d,%d), forceX: %10.3f, forceY: %10.3f", i, (int)(new_bodies[i].mass), (int)new_bodies[i].x[old], (int)new_bodies[i].y[old], _XF(i), _YF(i));
            }
        }*/
        compute_forces();

        /*if(printed <= 1 && myid == 0) {
            printf("B -> %d\n", myid);
            for (i = 0; i < bodyCt; i++) {
                printf("\nbody: %d, mass: %d, pos: (%d,%d), forceX: %10.3f, forceY: %10.3f", i, (int)(new_bodies[i].mass), (int)new_bodies[i].x[old], (int)new_bodies[i].y[old], _XF(i), _YF(i));
            }
        }*/

        new_forces2 = malloc(sizeof(forceType) * bodyCt);
        MPI_Allreduce(new_forces, new_forces2, bodyCt, mpi_force_type, mpi_sum, MPI_COMM_WORLD);
        free(new_forces);
        new_forces = new_forces2;

        /*if(printed <= 1 && myid == 0) {
            printf("C -> %d\n", myid);
            for (i = 0; i < bodyCt; i++) {
                printf("\nbody: %d, mass: %d, pos: (%d,%d), forceX: %10.3f, forceY: %10.3f", i, (int)(new_bodies[i].mass), (int)new_bodies[i].x[old], (int)new_bodies[i].y[old], _XF(i), _YF(i));
            }
            printed++;
        }*/
        compute_velocities();
        compute_positions();

        old ^= 1;

        /*for (b = displs[myid]; b < displs[myid] + bodies_per_proc[myid]; ++b) {
            rec_bodies[cont] = new_bodies[b];
            cont++;
        }*/

        /*if(printed <= 1) {
            printf("__ID__2: %d:\n", myid);
            print_mine();
            printed++;
        }*/

        /*Time for a display update?*/
        /*if (secsup > 0 && (time(0) - lastup) > secsup) {
            display();
            msync(map, fsize, MS_SYNC);
            lastup = time(0);
        }*/
    }
    if(0 == myid) {
        if(gettimeofday(&end, 0) != 0) {
            fprintf(stderr, "could not do timing\n");
            exit(1);
        }
        rtime = (end.tv_sec + (end.tv_usec / 1000000.0)) -
                (start.tv_sec + (start.tv_usec / 1000000.0));

        fprintf(stderr, "N-body took %10.3f seconds\n", rtime);
    }


    /*for (b = displs[myid]; b < displs[myid] + bodies_per_proc[myid]; ++b) {
        rec_bodies[cont] = new_bodies[b];
        cont++;
    }
    new_bodies = malloc(sizeof(bodyType) * bodyCt);
    MPI_Gatherv(rec_bodies, bodies_per_proc[myid], mpi_force_type, new_bodies, bodies_per_proc, displs, mpi_force_type, 0, MPI_COMM_WORLD);
    */
    if(0 == myid) {
        print();
        fprintf(stderr, "fine\n");
    }





    MPI_Finalize();

    free(bodies_per_proc);
    free(displs);
    free(new_bodies);
    free(rec_bodies);

    return 0;
}


/*

if(gettimeofday(&start, 0) != 0) {
        fprintf(stderr, "could not do timing\n");
        exit(1);
    }
    while (steps--) {
        clear_forces();
        compute_forces();
        compute_velocities();
        compute_positions();

        old ^= 1;

        if (secsup > 0 && (time(0) - lastup) > secsup) {
            display();
            msync(map, fsize, MS_SYNC);
            lastup = time(0);
        }
    }

    if(gettimeofday(&end, 0) != 0) {
        fprintf(stderr, "could not do timing\n");
        exit(1);
    }

    rtime = (end.tv_sec + (end.tv_usec / 1000000.0)) -
            (start.tv_sec + (start.tv_usec / 1000000.0));

    fprintf(stderr, "N-body took %10.3f seconds\n", rtime);

    print();

    return 0;
}
*/
