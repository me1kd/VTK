/*=========================================================================

  Program:   Visualization Toolkit
  Module:    vtkStructuredExtent.cxx
  Language:  C++
  Date:      $Date$
  Version:   $Revision$


Copyright (c) 1993-1998 Ken Martin, Will Schroeder, Bill Lorensen.

This software is copyrighted by Ken Martin, Will Schroeder and Bill Lorensen.
The following terms apply to all files associated with the software unless
explicitly disclaimed in individual files. This copyright specifically does
not apply to the related textbook "The Visualization Toolkit" ISBN
013199837-4 published by Prentice Hall which is covered by its own copyright.

The authors hereby grant permission to use, copy, and distribute this
software and its documentation for any purpose, provided that existing
copyright notices are retained in all copies and that this notice is included
verbatim in any distributions. Additionally, the authors grant permission to
modify this software and its documentation for any purpose, provided that
such modifications are not distributed without the explicit consent of the
authors and that existing copyright notices are retained in all copies. Some
of the algorithms implemented by this software are patented, observe all
applicable patent law.

IN NO EVENT SHALL THE AUTHORS OR DISTRIBUTORS BE LIABLE TO ANY PARTY FOR
DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES ARISING OUT
OF THE USE OF THIS SOFTWARE, ITS DOCUMENTATION, OR ANY DERIVATIVES THEREOF,
EVEN IF THE AUTHORS HAVE BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

THE AUTHORS AND DISTRIBUTORS SPECIFICALLY DISCLAIM ANY WARRANTIES, INCLUDING,
BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
PARTICULAR PURPOSE, AND NON-INFRINGEMENT.  THIS SOFTWARE IS PROVIDED ON AN
"AS IS" BASIS, AND THE AUTHORS AND DISTRIBUTORS HAVE NO OBLIGATION TO PROVIDE
MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.


=========================================================================*/

#include "vtkStructuredExtent.h"
#include "vtkUnstructuredExtent.h"
#include "vtkObjectFactory.h"



//------------------------------------------------------------------------------
vtkStructuredExtent* vtkStructuredExtent::New()
{
  // First try to create the object from the vtkObjectFactory
  vtkObject* ret = vtkObjectFactory::CreateInstance("vtkStructuredExtent");
  if(ret)
    {
    return (vtkStructuredExtent*)ret;
    }
  // If the factory was unable to create the object, then create it here.
  return new vtkStructuredExtent;
}




//----------------------------------------------------------------------------
// Construct a new vtkStructuredExtent 
vtkStructuredExtent::vtkStructuredExtent()
{
  int idx;
  
  for (idx = 0; idx < 3; ++idx)
    {
    this->Extent[idx*2] = -VTK_LARGE_INTEGER;
    this->Extent[idx*2+1] = VTK_LARGE_INTEGER;
    }
}



//----------------------------------------------------------------------------
void vtkStructuredExtent::PrintSelf(ostream& os, vtkIndent indent)
{
  vtkExtent::PrintSelf(os, indent);
  os << indent << "Extent: " << this->Extent[0] << ", " << this->Extent[1] 
     << ", " << this->Extent[2] << ", " << this->Extent[3] 
     << ", " << this->Extent[4] << ", " << this->Extent[5] << endl;
}


//----------------------------------------------------------------------------
void vtkStructuredExtent::Copy(vtkExtent *in)
{
  // call the supperclasses copy
  this->vtkExtent::Copy(in);
  
  // WARNING!!!
  // This logic only works because of the simple class hierachy.
  // If you subclass off this extent, ClassName cannot be used.
  if (strcmp(in->GetClassName(), "vtkStructuredExtent") == 0)
    {
    int idx;
    vtkStructuredExtent *e = (vtkStructuredExtent*)(in);
  
    for (idx = 0; idx < 6; ++idx)
      {
      this->Extent[idx] = e->Extent[idx];
      }
    } 
}

//----------------------------------------------------------------------------
void vtkStructuredExtent::WriteSelf(ostream& os)
{
  this->vtkExtent::WriteSelf(os);

  os << this->Extent[0] << " " << this->Extent[1] << " " 
     << this->Extent[2] << " " << this->Extent[3] << " " 
     << this->Extent[4] << " " << this->Extent[5] << " " ;
}

//----------------------------------------------------------------------------
void vtkStructuredExtent::ReadSelf(istream& is)
{
  this->vtkExtent::ReadSelf(is);

  is >> this->Extent[0] >> this->Extent[1] 
     >> this->Extent[2] >> this->Extent[3] 
     >> this->Extent[4] >> this->Extent[5] ;
}

