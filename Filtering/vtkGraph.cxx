/*=========================================================================

  Program:   Visualization Toolkit
  Module:    vtkGraph.cxx

  Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
  All rights reserved.
  See Copyright.txt or http://www.kitware.com/Copyright.htm for details.

     This software is distributed WITHOUT ANY WARRANTY; without even
     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
     PURPOSE.  See the above copyright notice for more information.

=========================================================================*/
/*----------------------------------------------------------------------------
 Copyright (c) Sandia Corporation
 See Copyright.txt or http://www.paraview.org/HTML/Copyright.html for details.
----------------------------------------------------------------------------*/

#include "vtkGraph.h"

#include "vtkAdjacentVertexIterator.h"
#include "vtkDataSetAttributes.h"
#include "vtkDistributedGraphHelper.h"
#include "vtkEdgeListIterator.h"
#include "vtkGraphInternals.h"
#include "vtkIdTypeArray.h"
#include "vtkInEdgeIterator.h"
#include "vtkInformation.h"
#include "vtkInformationVector.h"
#include "vtkMath.h"
#include "vtkObjectFactory.h"
#include "vtkOutEdgeIterator.h"
#include "vtkPoints.h"
#include "vtkVertexListIterator.h"

#include <vtksys/stl/vector>

using vtksys_stl::vector;

double vtkGraph::DefaultPoint[3] = {0, 0, 0};

//----------------------------------------------------------------------------
// class vtkGraph
//----------------------------------------------------------------------------
vtkCxxSetObjectMacro(vtkGraph, Points, vtkPoints);
vtkCxxSetObjectMacro(vtkGraph, Internals, vtkGraphInternals);
vtkCxxRevisionMacro(vtkGraph, "1.12.4.4");
//----------------------------------------------------------------------------
vtkGraph::vtkGraph()
{
  this->VertexData = vtkDataSetAttributes::New();
  this->EdgeData = vtkDataSetAttributes::New();
  this->Points = 0;
  vtkMath::UninitializeBounds(this->Bounds);

  this->Information->Set(vtkDataObject::DATA_EXTENT_TYPE(), VTK_PIECES_EXTENT);
  this->Information->Set(vtkDataObject::DATA_PIECE_NUMBER(), -1);
  this->Information->Set(vtkDataObject::DATA_NUMBER_OF_PIECES(), 1);
  this->Information->Set(vtkDataObject::DATA_NUMBER_OF_GHOST_LEVELS(), 0);

  this->Internals = vtkGraphInternals::New();
}

//----------------------------------------------------------------------------
vtkGraph::~vtkGraph()
{
  this->VertexData->Delete();
  this->EdgeData->Delete();
  if (this->Points)
    {
    this->Points->Delete();
    }
  this->Internals->Delete();
}

//----------------------------------------------------------------------------
double *vtkGraph::GetPoint(vtkIdType ptId)
{
  if (this->Points)
    {
    return this->Points->GetPoint(ptId);
    }
  return this->DefaultPoint;
}

//----------------------------------------------------------------------------
void vtkGraph::GetPoint(vtkIdType ptId, double x[3])
{
  if (this->Points)
    {
    int myRank = this->Information->Get(vtkDataObject::DATA_PIECE_NUMBER());
    if (myRank != -1 && myRank != this->GetVertexOwner(ptId))
      {
      vtkErrorMacro("vtkGraph cannot retrieve a point for a non-local vertex");
      }

    this->Points->GetPoint(this->GetVertexIndex(ptId), x);
    }
  else
    {
    for (int i = 0; i < 3; i++)
      {
      x[i] = this->DefaultPoint[i];
      }
    }
}

//----------------------------------------------------------------------------
vtkPoints *vtkGraph::GetPoints()
{
  if (!this->Points)
    {
    this->Points = vtkPoints::New();
    }
  if (this->Points->GetNumberOfPoints() != this->GetNumberOfVertices())
    {
    this->Points->SetNumberOfPoints(this->GetNumberOfVertices());
    for (vtkIdType i = 0; i < this->GetNumberOfVertices(); i++)
      {
      this->Points->SetPoint(i, 0, 0, 0);
      }
    }
  return this->Points;
}

//----------------------------------------------------------------------------
void vtkGraph::ComputeBounds()
{
  double *bounds;

  if ( this->Points )
    {
    bounds = this->Points->GetBounds();
    for (int i=0; i<6; i++)
      {
      this->Bounds[i] = bounds[i];
      }
    // TODO: how to compute the bounds for a distributed graph?
    this->ComputeTime.Modified();
    }
}

//----------------------------------------------------------------------------
double *vtkGraph::GetBounds()
{
  this->ComputeBounds();
  return this->Bounds;
}

//----------------------------------------------------------------------------
void vtkGraph::GetBounds(double bounds[6])
{
  this->ComputeBounds();
  for (int i=0; i<6; i++)
    {
    bounds[i] = this->Bounds[i];
    }
}

//----------------------------------------------------------------------------
unsigned long int vtkGraph::GetMTime()
{
  unsigned long int doTime = vtkDataObject::GetMTime();

  if ( this->VertexData->GetMTime() > doTime )
    {
    doTime = this->VertexData->GetMTime();
    }
  if ( this->EdgeData->GetMTime() > doTime )
    {
    doTime = this->EdgeData->GetMTime();
    }
  if ( this->Points ) 
    {
    if ( this->Points->GetMTime() > doTime )
      {
      doTime = this->Points->GetMTime();
      }
    }

  return doTime;
}

//----------------------------------------------------------------------------
void vtkGraph::Initialize()
{
  this->ForceOwnership();
  Superclass::Initialize();
  this->EdgeData->Initialize();
  this->VertexData->Initialize();
  this->Internals->NumberOfEdges = 0;
  this->Internals->Adjacency.clear();
}

//----------------------------------------------------------------------------
void vtkGraph::GetOutEdges(vtkIdType v, vtkOutEdgeIterator *it)
{
  int myRank = this->Information->Get(vtkDataObject::DATA_PIECE_NUMBER());
  if (myRank != -1 && myRank != this->GetVertexOwner(v))
    {
    vtkErrorMacro("vtkGraph cannot retrieve the out edges for a non-local vertex");
    }

  if (it)
    {
    it->Initialize(this, v);
    }
}

//----------------------------------------------------------------------------
void vtkGraph::GetOutEdges(vtkIdType v, const vtkOutEdgeType *& edges, vtkIdType & nedges)
{
  int myRank = this->Information->Get(vtkDataObject::DATA_PIECE_NUMBER());
  if (myRank != -1 && myRank != this->GetVertexOwner(v))
    {
    vtkErrorMacro("vtkGraph cannot retrieve the out edges for a non-local vertex");
    }

  nedges = this->Internals->Adjacency[this->GetVertexIndex(v)].OutEdges.size();
  if (nedges > 0)
    {
    edges = &(this->Internals->Adjacency[this->GetVertexIndex(v)].OutEdges[0]);
    }
  else
    {
    edges = 0;
    }
}

//----------------------------------------------------------------------------
vtkIdType vtkGraph::GetOutDegree(vtkIdType v)
{
  int myRank = this->Information->Get(vtkDataObject::DATA_PIECE_NUMBER());
  if (myRank != -1 && myRank != this->GetVertexOwner(v))
    {
    vtkErrorMacro("vtkGraph cannot determine the out degree for a non-local vertex");
    }
  return this->Internals->Adjacency[this->GetVertexIndex(v)].OutEdges.size();
}

//----------------------------------------------------------------------------
vtkIdType vtkGraph::GetDegree(vtkIdType v)
{
  int myRank = this->Information->Get(vtkDataObject::DATA_PIECE_NUMBER());
  if (myRank != -1 && myRank != this->GetVertexOwner(v))
    {
    vtkErrorMacro("vtkGraph cannot determine the degree for a non-local vertex");
    }
  return this->Internals->Adjacency[this->GetVertexIndex(v)].InEdges.size() +
         this->Internals->Adjacency[this->GetVertexIndex(v)].OutEdges.size();
}

//----------------------------------------------------------------------------
void vtkGraph::GetInEdges(vtkIdType v, vtkInEdgeIterator *it)
{
  int myRank = this->Information->Get(vtkDataObject::DATA_PIECE_NUMBER());
  if (myRank != -1 && myRank != this->GetVertexOwner(v))
    {
    vtkErrorMacro("vtkGraph cannot retrieve the in edges for a non-local vertex");
    }
  if (it)
    {
    it->Initialize(this, v);
    }
}

//----------------------------------------------------------------------------
void vtkGraph::GetInEdges(vtkIdType v, const vtkInEdgeType *& edges, vtkIdType & nedges)
{
  int myRank = this->Information->Get(vtkDataObject::DATA_PIECE_NUMBER());
  if (myRank != -1 && myRank != this->GetVertexOwner(v))
    {
    vtkErrorMacro("vtkGraph cannot retrieve the in edges for a non-local vertex");
    }
  nedges = this->Internals->Adjacency[this->GetVertexIndex(v)].InEdges.size();
  if (nedges > 0)
    {
    edges = &(this->Internals->Adjacency[this->GetVertexIndex(v)].InEdges[0]);
    }
  else
    {
    edges = 0;
    }
}

//----------------------------------------------------------------------------
vtkIdType vtkGraph::GetInDegree(vtkIdType v)
{
  int myRank = this->Information->Get(vtkDataObject::DATA_PIECE_NUMBER());
  if (myRank != -1 && myRank != this->GetVertexOwner(v))
    {
    vtkErrorMacro("vtkGraph cannot determine the in degree for a non-local vertex");
    }
  return this->Internals->Adjacency[this->GetVertexIndex(v)].InEdges.size();
}

//----------------------------------------------------------------------------
void vtkGraph::GetAdjacentVertices(vtkIdType v, vtkAdjacentVertexIterator *it)
{
  int myRank = this->Information->Get(vtkDataObject::DATA_PIECE_NUMBER());
  if (myRank != -1 && myRank != this->GetVertexOwner(v))
    {
    vtkErrorMacro("vtkGraph cannot retrieve the adjacent vertices for a non-local vertex");
    }

  if (it)
    {
    it->Initialize(this, v);
    }
}

//----------------------------------------------------------------------------
void vtkGraph::GetEdges(vtkEdgeListIterator *it)
{
  if (it)
    {
    it->SetGraph(this);
    }
}

//----------------------------------------------------------------------------
vtkIdType vtkGraph::GetNumberOfEdges()
{
  return this->Internals->NumberOfEdges;
}

//----------------------------------------------------------------------------
void vtkGraph::GetVertices(vtkVertexListIterator *it)
{
  if (it)
    {
    it->SetGraph(this);
    }
}

//----------------------------------------------------------------------------
vtkIdType vtkGraph::GetNumberOfVertices()
{
  return this->Internals->Adjacency.size();
}

//----------------------------------------------------------------------------
vtkIdType vtkGraph::GetVertexOwner(vtkIdType v) const
{
  vtkIdType owner = 0;
  int numProcs = this->Information->Get(DATA_NUMBER_OF_PIECES());

  if (numProcs > 1)
    {
    int idNumBits = sizeof(vtkIdType) << 3;  // numBytes * 8
    int numBits = ceil(log2(numProcs));
    // TODO: the use of "unsigned long long" here is wrong; we need
    // the unsigned equivalent of vtkIdType, otherwise sign extension
    // kills us.
    owner = (unsigned long long)v >> (idNumBits-numBits); 
    }
  
  return owner;
}

//----------------------------------------------------------------------------
vtkIdType vtkGraph::GetVertexIndex(vtkIdType v) const
{
  vtkIdType index = v;
  int numProcs = this->Information->Get(DATA_NUMBER_OF_PIECES());

  if (numProcs > 1)
    {
      int numBits = ceil(log2(numProcs));
      index = (v << numBits) >> numBits;
    }
  
  return index;
}

//----------------------------------------------------------------------------
vtkIdType vtkGraph::GetEdgeOwner(vtkIdType e_id) const
{
  vtkIdType owner = 0;
  int numProcs = this->Information->Get(DATA_NUMBER_OF_PIECES());
  
  if (numProcs > 1)
    {
      int idNumBits = sizeof(vtkIdType) << 3;  // numBytes * 8
      int numBits = ceil(log2(numProcs));
    // TODO: the use of "unsigned long long" here is wrong; we need
    // the unsigned equivalent of vtkIdType, otherwise sign extension
    // kills us.
      owner = (unsigned long long)e_id >> (idNumBits-numBits);
    }
  
  return owner;
}

//----------------------------------------------------------------------------
vtkIdType vtkGraph::GetEdgeIndex(vtkIdType e_id) const
{
  vtkIdType index = e_id;
  int numProcs = this->Information->Get(DATA_NUMBER_OF_PIECES());

  if (numProcs > 1)
  {
    int numBits = ceil(log2(numProcs));
    index = (e_id << numBits) >> numBits;
  }
  
  return index;
}

//----------------------------------------------------------------------------
vtkIdType vtkGraph::MakeDistributedId(int owner, vtkIdType local)
{
  int numProcs = this->Information->Get(DATA_NUMBER_OF_PIECES());

  if (numProcs > 1)
    {
    int procBits = ceil(log2(numProcs));
    return (vtkIdType(owner) << ((sizeof(vtkIdType) * CHAR_BIT) - procBits))
           | local;
    }
  
  return local;
}

//----------------------------------------------------------------------------
void vtkGraph::SetDistributedGraphHelper(vtkDistributedGraphHelper *helper)
{
  if (this->Internals->DistributedHelper)
    this->Internals->DistributedHelper->AttachToGraph(0);

  this->Internals->DistributedHelper = helper;
  if (this->Internals->DistributedHelper)
    this->Internals->DistributedHelper->AttachToGraph(this);
}

//----------------------------------------------------------------------------
vtkDistributedGraphHelper *vtkGraph::GetDistributedGraphHelper()
{
  return this->Internals->DistributedHelper;
}

//----------------------------------------------------------------------------
bool vtkGraph::CheckedShallowCopy(vtkGraph *g)
{
  if (!g)
    {
    return false;
    }
  bool valid = this->IsStructureValid(g);
  if (valid)
    {
    this->CopyInternal(g, false);
    }
  return valid;
}

//----------------------------------------------------------------------------
bool vtkGraph::CheckedDeepCopy(vtkGraph *g)
{
  if (!g)
    {
    return false;
    }
  bool valid = this->IsStructureValid(g);
  if (valid)
    {
    this->CopyInternal(g, true);
    }
  return valid;
}

//----------------------------------------------------------------------------
void vtkGraph::ShallowCopy(vtkDataObject *obj)
{
  vtkGraph *g = vtkGraph::SafeDownCast(obj);
  if (!g)
    {
    vtkErrorMacro("Can only shallow copy from vtkGraph subclass.");
    return;
    }
  bool valid = this->IsStructureValid(g);
  if (valid)
    {
    this->CopyInternal(g, false);
    }
  else
    {
    vtkErrorMacro("Invalid graph structure for this type of graph.");
    }
}

//----------------------------------------------------------------------------
void vtkGraph::DeepCopy(vtkDataObject *obj)
{
  vtkGraph *g = vtkGraph::SafeDownCast(obj);
  if (!g)
    {
    vtkErrorMacro("Can only shallow copy from vtkGraph subclass.");
    return;
    }
  bool valid = this->IsStructureValid(g);
  if (valid)
    {
    this->CopyInternal(g, true);
    }
  else
    {
    vtkErrorMacro("Invalid graph structure for this type of graph.");
    }
}

//----------------------------------------------------------------------------
void vtkGraph::CopyStructure(vtkGraph *g)
{
  // Copy on write.
  this->SetInternals(g->Internals);
  if (g->Points)
    {
    if (!this->Points)
      {
      this->Points = vtkPoints::New();
      }
    this->Points->ShallowCopy(g->Points);
    }
  else if (this->Points)
    {
    this->Points->Delete();
    this->Points = 0;
    }
}

//----------------------------------------------------------------------------
void vtkGraph::CopyInternal(vtkGraph *g, bool deep)
{
  if (deep)
    {
    vtkDataObject::DeepCopy(g);
    }
  else
    {
    vtkDataObject::ShallowCopy(g);
    }
  // Copy on write.
  this->SetInternals(g->Internals);
  if (deep)
    {
    this->EdgeData->DeepCopy(g->EdgeData);
    this->VertexData->DeepCopy(g->VertexData);
    }
  else
    {
    this->EdgeData->ShallowCopy(g->EdgeData);
    this->VertexData->ShallowCopy(g->VertexData);
    }
  if (g->Points)
    {
    if (!this->Points)
      {
      this->Points = vtkPoints::New();
      }
    if (deep)
      {
      this->Points->DeepCopy(g->Points);
      }
    else
      {
      this->Points->ShallowCopy(g->Points);
      }
    }
  else if (this->Points)
    {
    this->Points->Delete();
    this->Points = 0;
    }
}

//----------------------------------------------------------------------------
void vtkGraph::Squeeze()
{
  if ( this->Points )
    {
    this->Points->Squeeze();
    }
  this->EdgeData->Squeeze();
  this->VertexData->Squeeze();
}

//----------------------------------------------------------------------------
vtkGraph *vtkGraph::GetData(vtkInformation *info)
{
  return info? vtkGraph::SafeDownCast(info->Get(DATA_OBJECT())) : 0;
}

//----------------------------------------------------------------------------
vtkGraph *vtkGraph::GetData(vtkInformationVector *v, int i)
{
  return vtkGraph::GetData(v->GetInformationObject(i));
}

//----------------------------------------------------------------------------
vtkIdType vtkGraph::AddVertexInternal()
{
  this->ForceOwnership();
  this->Internals->Adjacency.push_back(vtkVertexAdjacencyList());
  return this->Internals->Adjacency.size() - 1;
}

//----------------------------------------------------------------------------
vtkEdgeType vtkGraph::AddEdgeInternal(vtkIdType u, vtkIdType v, bool directed)
{
  this->ForceOwnership();
  if (this->Internals->DistributedHelper)
    {
    return this->Internals->DistributedHelper->AddEdgeInternal(u, v, directed);
    }

  vtkIdType edgeId = this->Internals->NumberOfEdges;
  this->Internals->NumberOfEdges++;
  this->Internals->Adjacency[u].OutEdges.push_back(vtkOutEdgeType(v, edgeId));
  if (directed)
    {
    this->Internals->Adjacency[v].InEdges.push_back(vtkInEdgeType(u, edgeId));
    }
  else if (u != v)
    {
    // Avoid storing self-loops twice in undirected graphs.
    this->Internals->Adjacency[v].OutEdges.push_back(vtkOutEdgeType(u, edgeId));
    }
  return vtkEdgeType(u, v, edgeId);
}

//----------------------------------------------------------------------------
void vtkGraph::ReorderOutVertices(vtkIdType v, vtkIdTypeArray *vertices)
{
  int myRank = this->Information->Get(vtkDataObject::DATA_PIECE_NUMBER());
  if (myRank != -1 && myRank != this->GetVertexOwner(v))
    {
    vtkErrorMacro("vtkGraph cannot reorder the out vertices for a non-local vertex");
    return;
    }

  this->ForceOwnership();
  vtksys_stl::vector<vtkOutEdgeType> outEdges;
  vtksys_stl::vector<vtkOutEdgeType>::iterator it, itEnd;
  itEnd = this->Internals->Adjacency[v].OutEdges.end();
  for (vtkIdType i = 0; i < vertices->GetNumberOfTuples(); ++i)
    {
    vtkIdType vert = vertices->GetValue(i);
    // Find the matching edge
    for (it = this->Internals->Adjacency[v].OutEdges.begin(); it != itEnd; ++it)
      {
      if (it->Target == vert)
        {
        outEdges.push_back(*it);
        break;
        }
      }
    }
  if (outEdges.size() != this->Internals->Adjacency[v].OutEdges.size())
    {
    vtkErrorMacro("Invalid reorder list.");
    return;
    }
  this->Internals->Adjacency[v].OutEdges = outEdges;
}

//----------------------------------------------------------------------------
bool vtkGraph::IsSameStructure(vtkGraph *other)
{
  return (this->Internals == other->Internals);
}

//----------------------------------------------------------------------------
vtkGraphInternals* vtkGraph::GetGraphInternals(bool modifying)
{
  if (modifying)
    {
    this->ForceOwnership();
    }
  return this->Internals;
}

//----------------------------------------------------------------------------
void vtkGraph::ForceOwnership()
{
  // If the reference count == 1, we own it and can change it.
  // If the reference count > 1, we must make a copy to avoid
  // changing the structure of other graphs.
  if (this->Internals->GetReferenceCount() > 1)
    {
    vtkGraphInternals *internals = vtkGraphInternals::New();
    internals->Adjacency = this->Internals->Adjacency;
    internals->NumberOfEdges = this->Internals->NumberOfEdges;
    this->SetInternals(internals);
    internals->Delete();
    }
}

//----------------------------------------------------------------------------
void vtkGraph::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os,indent);
  os << indent << "VertexData: " << (this->VertexData ? "" : "(none)") << endl;
  if (this->VertexData)
    {
    this->VertexData->PrintSelf(os, indent.GetNextIndent());
    }
  os << indent << "EdgeData: " << (this->EdgeData ? "" : "(none)") << endl;
  if (this->EdgeData)
    {
    this->EdgeData->PrintSelf(os, indent.GetNextIndent());
    }
}

//----------------------------------------------------------------------------
// Supporting operators
//----------------------------------------------------------------------------

//----------------------------------------------------------------------------
bool operator==(vtkEdgeBase e1, vtkEdgeBase e2)
{
  return e1.Id == e2.Id;
}

//----------------------------------------------------------------------------
bool operator!=(vtkEdgeBase e1, vtkEdgeBase e2)
{
  return e1.Id != e2.Id;
}

//----------------------------------------------------------------------------
ostream& operator<<(ostream& out, vtkEdgeBase e)
{
  return out << e.Id;
}
