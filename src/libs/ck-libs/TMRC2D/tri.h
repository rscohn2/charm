// Triangular Mesh Refinement Framework - 2D (TMR)
// Created by: Terry L. Wilmarth
#ifndef TRI_H
#define TRI_H

#include <vector.h>
#include "charm++.h"
#include "tcharm.h"
#include "charm-api.h"
#include "ref.h"
#include "node.h"
#include "edge.h"
#include "element.h"
#include "refine.decl.h"
#include "messages.h"
#include "mpi.h"

#define DEBUGREF(x) x
//#define DEBUGREF(x) 

// ------------------------ Global Read-only Data ---------------------------
extern CProxy_chunk mesh;
class chunk;
CtvExtern(chunk *, _refineChunk);


/**
 * The user inherits from this class to receive "split" calls,
 * and to be informed when the refinement is complete.
 */
class refineClient {
public:
  virtual ~refineClient() {}

  /**
   * This triangle of our chunk is being split along this edge.
   *
   * For our purposes, edges are numbered 0 (connecting nodes 0 and 1), 
   * 1 (connecting 1 and 2), and 2 (connecting 2 and 0).
   * 
   * Taking as A and B the (triangle-order) nodes of the splitting edge:
   *
   *     ____ A          ____ A   
   *   /      |        /      |  
   *  C       |  =>   C ----- D  <- new node
   *   \      |        \      |   
   *     ---- B          ---- B 
   *
   *   The original triangle should be shrunk to ADC; while a 
   * new triangle should be inserted at DBC.
   *
   *   The new node D's location should equal A*(1-frac)+B*frac.
   * For a simple splitter, frac will always be 0.5.
   *
   *   If nodes A and B are shared with some other processor,
   * that processor will also receive a "split" call for the
   * same edge.  If nodes A and B are shared by some other local
   * triangle, that triangle will also receive a "split" call
   * for the same edge.  
   *
   * Client's responsibilities:
   *   -Add the new node D.  Since both sides of a shared edge
   *      will receive a "split" call, you must ensure the node is
   *      not added twice.
   *   -Update connectivity for source triangle
   *   -Add new triangle DBC.
   */
  // OBSOLETE
  virtual void split(int triNo,int edgeOfTri,int movingNode,double frac) {};
  // Split element triNo on edgeOfTri at frac; movingNode specifies
  // which side new element is added to (it moves to the new location
  // on edgeOfTri; frac indicates if this split is on the boundary and
  // if it is the first or second half of the split operation
  virtual void split(int triNo,int edgeOfTri,int movingNode,double frac,int flag) {};
  // Collapse the edge between of elemId1 between endpoints nodeToKeep
  // and nodeToDelete.  nodeToKeep's coordinates are updated to newX
  // and newY, while nodeToDelete is removed and all references to it
  // are replaced by references to nodeToKeep
  virtual void collapse(int elemId, int nodeToKeep, int nodeToDelete, double newX, double newY, int flag) {};
  // update nodeID with new coordinates newX and newY
  virtual void nodeUpdate(int nodeID, double newX, double newY){};
  // replace oldNodeID with newNodeID on element elementID and delete oldNodeID
  virtual void nodeReplaceDelete(int elementID, int relnodeID, int oldNodeID, int newNodeID){};
 
 /*
 // delete node entry at nodeID index
  virtual void nodeDelete(int nodeID){};
	*/
};

class refineResults; //Used by refinement API to store intermediate results
class coarsenResults; 

typedef struct prioLockStruct {
  int holderIdx;
  int holderCid;
  double prio;
  prioLockStruct *next;
} *prioLockRequests;

// ---------------------------- Chare Arrays -------------------------------
class chunk : public TCharmClient1D {
  // current sizes of arrays allocated for the mesh
  int sizeElements, sizeEdges, sizeNodes;
  // first empty slot in each mesh array
  int firstFreeElement, firstFreeEdge, firstFreeNode;

  int edgesSent, edgesRecvd, first;

  void setupThreadPrivate(CthThread forThread) {
    CtvAccessOther(forThread, _refineChunk) = this;
  }
  // information about connectivity and location of ghost elements
  int *conn, *gid, additions;
  
  // debug_counter is used to print successive snapshots of the chunk
  // and match them up to other chunk snapshots; refineInProgress
  // flags that the refinement loop is active; modified flags that a
  // target area for some element on this chunk has been modified
  int debug_counter, refineInProgress, coarsenInProgress, modified;

  // meshLock is used to lock the mesh for expansion; if meshlock is
  // zero, the mesh can be either accessed or locked; accesses to the
  // mesh (by a chunk method) decrement the lock, and when the
  // accesses are complete, the lock is incremented; when an expansion
  // of the mesh is required, the meshExpandFlag is set, indicating
  // that no more accesses will be allowed to the mesh until the
  // adjuster gets control and completes the expansion; when the
  // adjuster gets control, it sets meshLock to 1 and when it is
  // finished, it resets both variables to zero.  See methods below.
  int meshLock, meshExpandFlag;

  // private helper methods used by FEM interface functions
  void deriveNodes();
  int edgeLocal(elemRef e1, elemRef e2);
  int getNbrRefOnEdge(int n1, int n2, int *conn, int nGhost, int *gid, 
		      int idx, elemRef *er);
  int hasEdge(int n1, int n2, int *conn, int idx);
  
 public:
  // Data fields for this chunk's array index, and counts of elements,
  // edges, and nodes located on this chunk; numGhosts is numElements
  // plus number of ghost elements surrounding this chunk
  int cid, numElements, numEdges, numNodes, numGhosts, numChunks;
  //meshID passed in from FEM framework.
  int meshID;
  FEM_Mesh *meshPtr;

  refineResults *refineResultsStorage;
  coarsenResults *coarsenResultsStorage;

  // the chunk's components, left public for sanity's sake
  std::vector<element> theElements;
  std::vector<edge> theEdges;
  std::vector<node> theNodes;

  // client to report refinement split information to
  refineClient *theClient;

  // range of occupied slots in each mesh array
  int elementSlots, edgeSlots, nodeSlots;

  int lock, lockHolderIdx, lockHolderCid, lockCount;
  double lockPrio;
  prioLockRequests lockList;

  // Basic constructors
  chunk(chunkMsg *);
  chunk(CkMigrateMessage *m) : TCharmClient1D(m) { };
  
  void sanityCheck(void);
 
  // entry methods
  void deriveBoundaries(int *conn, int *gid);
  void tweakMesh();
  void improveChunk();
  void improve();
  
  // deriveEdges creates nodes from the element & ghost info, then creates
  // unique edges for each adjacent pair of nodes
  void deriveEdges(int *conn, int *gid);

  // This initiates a refinement for a single element
  void refineElement(int idx, double area);
  // This loops through all elements performing refinements as needed
  void refiningElements();

  // This initiates a coarsening for a single element
  void coarsenElement(int idx, double area);
  // This loops through all elements performing coarsenings as needed
  void coarseningElements();

  // The following methods simply provide remote access to local data
  // See above for details of each
  intMsg *safeToMoveNode(int idx, double x, double y);
  splitOutMsg *split(int idx, elemRef e, node in, node fn);
  splitOutMsg *collapse(int idx, elemRef e, node kn, node dn, elemRef kNbr, 
			elemRef dNbr, edgeRef kEdge, edgeRef dEdge, 
			node opnode, node newN);
  void collapseHelp(int idx, edgeRef er, node n1, node n2);
  intMsg *nodeLockup(node n, double l, edgeRef start);
  intMsg *opnodeLockup(int elemID, double l, edgeRef e);
  void opnodeUnlock(int elemID, edgeRef e);
  int getNode(node n);
  void nodeUnlock(node n);
  void nodeUpdate(node n, node newNode);
  void nodeDelete(node n, node ndReplace);
  void nodeReplaceDelete(node kn, node dn, node nn);
  intMsg *isPending(int idx, objRef e);
  void checkPending(int idx, objRef aRef);
  void checkPending(int idx, objRef aRef1, objRef aRef2);
  void updateElement(int idx, objRef oldval, objRef newval);
  void updateElementEdge(int idx, objRef oldval, objRef newval);
  void updateReferences(int idx, objRef oldval, objRef newval);
  doubleMsg *getArea(int n);
  void resetEdge(int n);
  refMsg *getNbr(int idx, objRef aRef);
  void setTargetArea(int idx, double aDouble);
  void resetTargetArea(int idx, double aDouble);
  void reportPos(int idx, double x, double y);

  // meshLock methods
  void accessLock();  // waits until meshExpandFlag not set, then decs meshLock
  void releaseLock(); // incs meshLock
  void adjustFlag();  // sets meshExpandFlag
  void adjustLock();  // waits until meshLock is 0, then sets it to 1
  void adjustRelease();  // resets meshLock and meshExpandFlag to 0

  // used to print snapshots of all chunks at once (more or less)
  void print();

  // *** These methods are part of the interface with the FEM framework ***
  // create a chunk's mesh data
  void newMesh(int meshID_,int nEl, int nGhost,const int *conn_,const 
	       int *gid_, int nnodes, const int *boundaries, int idxOffset);
  // Sets target areas specified by desiredArea, starts refining
  void multipleRefine(double *desiredArea, refineClient *client);
  // Sets target areas specified by desiredArea, starts coarsening
  void multipleCoarsen(double *desiredArea, refineClient *client);
  // updateNodeCoords sets node coordinates, recalculates element areas
  void updateNodeCoords(int nNode, double *coord, int nEl);


  // add an edge on a remote chunk
  void addRemoteEdge(int elem, int localEdge, edgeRef er);

  // local methods
  // These access and set local flags
  void setModified() { modified = 1; }
  int isModified() { return modified; }
  void setRefining() { refineInProgress = 1; }
  int isRefining() { return refineInProgress; }

  // these methods allow for run-time additions/modifications to the chunk
  void allocMesh(int nEl);
  void adjustMesh();
  intMsg *addNode(node n);
  //int addNode(node n);
  edgeRef addEdge();
  elemRef addElement(int n1, int n2, int n3);
  elemRef addElement(int n1, int n2, int n3,
		      edgeRef er1, edgeRef er2, edgeRef er3);
  void removeNode(int n);
  void removeEdge(int n);
  void removeElement(int n);
  // prints a snapshot of the chunk to file
  void debug_print(int c);
  void out_print();
  void dump();

  intMsg *lockChunk(int lhc, int lhi, double prio);
  void unlockChunk(int lhc, int lhi);
  int lockLocalChunk(int lhc, int lhi, double prio);
  void unlockLocalChunk(int lhc, int lhi);
  void removeLock(int lhc, int lhi);
  void insertLock(int lhc, int lhi, double prio);
};

#endif
