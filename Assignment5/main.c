#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <sys/time.h>
#include <pthread.h>
#include "graphics/graphics.h"




/* Galaxy simulation using Barnes-Hut Algorithm
 * Input
 * N            number of stars/particles to simulate
 * filname      file to read the initial configuration from
 * nsteps       number of timesteps
 * delta_t      time step, set to 1e-5
 * theta_max    Controls the level of approximation of Barnes-Hut 
 * graphics     1 or 0: graphics on/off
 * n_threads    Number of threads
 */

#define E 0.001 
#define debug 0

typedef struct vectors
{
    double                  x;
    double                  y;    
}vector;

typedef struct quadtree
{
    struct  quadtree*   nw;
    struct  quadtree*   ne;
    struct  quadtree*   sw;
    struct  quadtree*   se;
    vector              c;    // center of mass
    vector              p;    // position, center of the bounding box
    double              m;
    float               h;    // half box width

} qt;

typedef struct thread_arg
{
    qt*       t;
    vector*   C;
    vector*   F;
    double*   M;  
    double    theta;
    double    G;
    long      rank;
    int       N;
    int       n_threads;
} t_arg;


void load_data(int N, vector* c, vector* v, double* m, double *b, char* filename);
void write_data(int N, vector* c, vector* v, double* m, double *b, char* filename);
qt *new_qt(vector c, double m);
void qt_add(qt **t, qt *tadd);
void qt_del(qt **t);
void qt_print(qt *t);
void force_BH_one_body(double G, vector* c, double *m, qt *t, vector *f, double theta);
int check_quadrant(vector c, vector p);
void * thread_force(void *arg);
int timer(void);


int main(int argc, char *argv[]) {

    if (argc != 8) {
        printf("Usage: ./galsim N filname nsteps delta_t theta_max graphics\n");
        return -1;
    }

    const int N = atoi(argv[1]);
    double const G = (double) 100/N;
    char *filename = argv[2];
    const int n = atoi(argv[3]);                  // number of time steps
    const double dt = atof(argv[4]);              // time step
    const float theta = atof(argv[5]);
    int graphics = atoi(argv[6]);           // graphics not available    
    const int n_threads = atoi(argv[7]);
    int starttime, ttime1=0, ttime2=0;                       // Wall time log


    vector *C = malloc(N * sizeof(vector));
    vector *V = malloc(N * sizeof(vector));
    double *M = malloc(N * sizeof(double));
    double *b = malloc(N * sizeof(double));    
    load_data(N, C, V, M, b, filename);

    const float circleRadius=0.002, circleColor=0.2;
    const int windowWidth=800;
    const float L = 1, W = 1;

    if (graphics) {
        InitializeGraphics(argv[0], windowWidth, windowWidth);
        SetCAxes(0,1);
        printf("Ctrl C to quit.\n");
    }

    // Stepping
    int i,j;
    for (j=0; j<n; j++) {
        qt *t = NULL;                           // reinitiate the tree root
        vector* F = calloc(N,sizeof(vector));   // reinitiate the force vector
        
        // build a new tree
        for (i=0; i<N; i++) {
            qt_add(&t, new_qt(C[i], M[i]));
        }
    #if debug
        qt_print(t);
    #endif

        t_arg thread_data[n_threads];
        void *status;

        long rank;
        starttime = timer();
        pthread_t threads[n_threads];
        for (rank=0; rank<n_threads; rank++) {
            thread_data[rank].rank = rank;
            thread_data[rank].t = t;
            thread_data[rank].C = C;
            thread_data[rank].F = F;
            thread_data[rank].M = M;
            thread_data[rank].theta = theta;
            thread_data[rank].G = G;
            thread_data[rank].N = N;
            thread_data[rank].n_threads = n_threads;
            pthread_create(&threads[rank], NULL, thread_force, (void *) &thread_data[rank]);
        }

        for (rank=0; rank<n_threads; rank++) {
            pthread_join(threads[rank], &status);
        }
        ttime1 += timer()-starttime;

        starttime = timer();
        for (i=0; i<N; i++) {
            // printf("(%f, %f)\n", F[i].x, F[i].y );
            // Update the velocity and position
            V[i].x +=  dt * F[i].x;
            V[i].y +=  dt * F[i].y;
            C[i].x +=  V[i].x * dt;
            C[i].y +=  V[i].y * dt;
        }
        ttime2 += timer()-starttime;

        if (graphics) {
            ClearScreen();
            for (i=0; i<N; i++) {
                DrawCircle(C[i].x, C[i].y, L, W, circleRadius, circleColor);
            }
            Refresh();
            usleep(4000);
        }

        free(F);
        qt_del(&t);
        free(t);           // Added since submission
    }

    if (graphics) {
        while(!CheckForQuit()) {
            usleep(200000);
        }
        FlushDisplay();
        CloseDisplay();
    }


    printf("Walltime for the threads in force calculation: %f\n", ttime1/1000000.0);
    printf("Walltime for the threads in position updates: %f\n", ttime2/1000000.0);


    char outputfile[11] = "result.gal";
    write_data(N, C, V, M, b, outputfile);

    free(C); free(V), free(M), free(b);
    pthread_exit(NULL);
    return 0;
}


void * thread_force(void *arg) {
   t_arg *my_data = (t_arg *) arg;
   qt *t        = my_data->t;
   vector *C    = my_data->C;
   vector *F    = my_data->F;
   double *M    = my_data->M;
   double theta = my_data->theta;
   double G     = my_data->G; 
   int N        = my_data->N;
   int n_threads= my_data->n_threads;
   long rank    = my_data->rank;

   long my_len, my_first_i, my_last_i;
   int i;
   my_len = (long) ceil( (double) N/n_threads);     // number of calculations in this thread
   my_first_i = my_len * rank;
   my_last_i = my_first_i + my_len;
   if (my_last_i > N) my_last_i = N; 

   for (i=my_first_i; i<my_last_i; i++) {
      force_BH_one_body(G, &(C[i]), &(M[i]), t, &(F[i]), theta);
   }
   pthread_exit(NULL);

}


/* Effectively computes the acceleration for one body */
void force_BH_one_body(double G, vector *c, double *m, qt *t, vector *f, double theta) {
    // node does not exist
    if (!t) {
        ;
    }

    // external branch
    else if ((t->nw==NULL) && (t->ne==NULL) && (t->sw==NULL) && (t->se==NULL)) {
        double rx = c->x - t->c.x;
        double ry = c->y - t->c.y;
        double r = sqrt(rx*rx + ry*ry);
        double comm = -G * t->m / ((r+E)*(r+E)*(r+E));

        f->x += comm * rx;
        f->y += comm * ry;
    }

    // internal branch
    else {                                 // Calcuate distance to the mid-point of the box
        double dx = c->x - t->p.x;
        double dy = c->y - t->p.y;
        double d = sqrt(dx*dx + dy*dy);

        if ((t->h)*2 < theta * d) {        // theta criterion satisfies, treat as one body

            double rx = c->x - t->c.x;
            double ry = c->y - t->c.y;
            double r = sqrt(rx*rx + ry*ry);
            // double comm = - G * b->m * t->m / ((r+E)*(r+E)*(r+E));
            double comm = - G * t->m / ((r+E)*(r+E)*(r+E));

            f->x += comm * rx;
            f->y += comm * ry;
        } 
        else {                             // theta criterion fails, traverse the subnodes
            force_BH_one_body(G, c, m, t->sw, f, theta);
            force_BH_one_body(G, c, m, t->nw, f, theta);
            force_BH_one_body(G, c, m, t->se, f, theta);
            force_BH_one_body(G, c, m, t->ne, f, theta);
        } 
    }
}

// Constructor for a new quadtree node
qt *new_qt(vector c, double m) {
    qt *newqt = malloc(sizeof(qt));
    newqt->c  = c;
    newqt->m  = m;
    newqt->nw = NULL;
    newqt->ne = NULL;
    newqt->sw = NULL;
    newqt->se = NULL;
    newqt->p.x=0.5;   // Initialize to always start from the root quadrant
    newqt->p.y=0.5;
    newqt->h=0.5;
    return newqt;
}



void qt_add(qt **t, qt *tadd){
     // Quadrant free
    if(!(*t)) {                    
        *t = tadd;
        return;
    } 

    // Current node is internal
    else if((((*t)->nw) || ((*t)->ne) || ((*t)->sw) || ((*t)->se))) {  

        double m1 = (*t)->m;
        
        // update current node mass and CoM
        (*t)->m += tadd->m;
        (*t)->c.x = (((*t)->c.x)*m1 + (tadd->c.x)*(tadd->m))/((*t)->m);
        (*t)->c.y = (((*t)->c.y)*m1 + (tadd->c.y)*(tadd->m))/((*t)->m);

        // update new child half box width
        tadd->h = ((*t)->h)/2;

        // Insert the new node into the correct quadrant
        switch (check_quadrant(tadd->c, (*t)->p)) {
            case 1: {
                tadd->p.x = ((*t)->p.x) - tadd->h;
                tadd->p.y = ((*t)->p.y) + tadd->h;
                qt_add(&((*t)->nw), tadd);
                break;
            }
            case 2: {
                tadd->p.x = ((*t)->p.x) + tadd->h;
                tadd->p.y = ((*t)->p.y) + tadd->h;
                qt_add(&((*t)->ne), tadd);
                break;
            }
            case 3: {
                tadd->p.x = ((*t)->p.x) - tadd->h;
                tadd->p.y = ((*t)->p.y) - tadd->h;
                qt_add(&((*t)->sw), tadd);break;
            }
            case 4: {
                tadd->p.x = ((*t)->p.x) + tadd->h;
                tadd->p.y = ((*t)->p.y) - tadd->h;
                qt_add(&((*t)->se), tadd);break;
            }
            default:break;
        }
        return;
    }

    // Current node is an external node
    else { 

        // copying the current node with half of the box width
        // ready to move down
        qt *tnew =  new_qt((*t)->c, (*t)->m);
        tnew->h = ((*t)->h)/2;

        // update new child half box width
        tadd->h = ((*t)->h)/2;

        // update current node with new mass and CoM
        double m1 = (*t)->m;
        (*t)->m += tadd->m;
        (*t)->c.x = (((*t)->c.x)*m1 + (tadd->c.x)*(tadd->m))/((*t)->m);
        (*t)->c.y = (((*t)->c.y)*m1 + (tadd->c.y)*(tadd->m))/((*t)->m);

        // Check the quadrant and insert the original node there
        switch (check_quadrant(tnew->c, (*t)->p)) {
            case 1: {
                tnew->p.x = ((*t)->p.x) - tnew->h;
                tnew->p.y = ((*t)->p.y) + tnew->h;
                (*t)->nw = tnew;
                break;
            }
            case 2: {
                tnew->p.x = ((*t)->p.x) + tnew->h;
                tnew->p.y = ((*t)->p.y) + tnew->h;
                (*t)->ne = tnew;
                break;
            }
            case 3: {
                tnew->p.x = ((*t)->p.x) - tnew->h;
                tnew->p.y = ((*t)->p.y) - tnew->h;
                ((*t)->sw) = tnew;
                break;
            }
            case 4: {
                tnew->p.x = ((*t)->p.x) + tnew->h;
                tnew->p.y = ((*t)->p.y) - tnew->h;
                (*t)->se = tnew;
                break;
            }
            default:break;
        }

        // Check the quadrant and insert the new node there
        switch (check_quadrant(tadd->c, (*t)->p)) {
            case 1: {
                tadd->p.x = ((*t)->p.x) - tadd->h;
                tadd->p.y = ((*t)->p.y) + tadd->h;
                qt_add(&((*t)->nw), tadd);
                break;
            }
            case 2: {
                tadd->p.x = ((*t)->p.x) + tadd->h;
                tadd->p.y = ((*t)->p.y) + tadd->h;
                qt_add(&((*t)->ne), tadd);
                break;
            }
            case 3: {
                tadd->p.x = ((*t)->p.x) - tadd->h;
                tadd->p.y = ((*t)->p.y) - tadd->h;
                qt_add(&((*t)->sw), tadd);break;
            }
            case 4: {
                tadd->p.x = ((*t)->p.x) + tadd->h;
                tadd->p.y = ((*t)->p.y) - tadd->h;
                qt_add(&((*t)->se), tadd);break;
            }
            default:break;
        }
        return;
    }
}

int check_quadrant(vector c, vector p) {
    //first quadrant NW
    if (c.x <= p.x && c.y >  p.y) return 1;  // NW
    if (c.x >  p.x && c.y >= p.y) return 2;  // NE
    if (c.x <  p.x && c.y <= p.y) return 3;  // SW
    if (c.x >= p.x && c.y <  p.y) return 4;  // SE
    else return 0;
}

void qt_del(qt **t) {
    // printf("*t address: %p\n", *t);
    // printf("*t-nw address: %p\n", (*t)->nw);
    // printf("*t-ne address: %p\n", (*t)->ne);
    // printf("*t-sw address: %p\n", (*t)->sw);
    // printf("*t-se address: %p\n", (*t)->se);
    if ((*t) != NULL) {

        if ((*t)->nw) {
            qt_del(&((*t)->nw));
            free((*t)->nw);
        }
        if ((*t)->ne) {
            qt_del(&((*t)->ne));
            free((*t)->ne);
        }
        if ((*t)->sw) {
            qt_del(&((*t)->sw));
            free((*t)->sw);
        }
        if ((*t)->se) {
            qt_del(&((*t)->se));
            free((*t)->se);
        }

        free(*t);
        *t = NULL;
    }   
}


/* Print out the quadtree, for debugging purpose*/
void qt_print(qt *t) {
    if (t == NULL) {;}
    else {
        printf("Node mass: %f\n", t->m);
        printf("Node position: (%f, %f)\n", t->p.x, t->p.y);
        printf("Node center of mass: (%f, %f)\n", t->c.x, t->c.y);
        printf("Bounding box height: %f\n", t->h);
        qt_print(t->nw);
        qt_print(t->ne);
        qt_print(t->sw);
        qt_print(t->se);
    }
}


void load_data(int N, vector* C, vector* V, double* M, double* b, char* filename) {
    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        printf("load_data error: failed to open input file '%s'.\n", filename);
        return;
    }
    int i;
    for (i=0; i<N; i++) {
        fread(&(C[i]), sizeof(double), 2, fp);
        fread(&(M[i]), sizeof(double), 1, fp);
        fread(&(V[i]), sizeof(double), 2, fp);
        fread(&(b[i]), sizeof(double), 1, fp);
    }
    fclose(fp);
}


void write_data(int N, vector* C, vector* V, double* M, double* b, char* filename) {
    FILE *fp = fopen(filename, "w");
    int i;
    for (i=0; i<N; i++) {
        fwrite(&(C[i]), sizeof(double), 2, fp);
        fwrite(&(M[i]), sizeof(double), 1, fp);
        fwrite(&(V[i]), sizeof(double), 2, fp);
        fwrite(&(b[i]), sizeof(double), 1, fp);
    }
    fclose(fp);
}


int timer(void)
{
  struct timeval tv;
  gettimeofday(&tv, (struct timezone*)0);
  return (tv.tv_sec*1000000+tv.tv_usec);
}



