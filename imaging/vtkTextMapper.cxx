/*=========================================================================

  Program:   Visualization Toolkit
  Module:    vtkTextMapper.cxx
  Language:  C++
  Date:      $Date$
  Version:   $Revision$
  Thanks:    Thanks to Matt Turek who developed this class.

Copyright (c) 1993-1995 Ken Martin, Will Schroeder, Bill Lorensen.

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
#include "vtkTextMapper.h"
#include "vtkObjectFactory.h"

#ifdef _WIN32
  #include "vtkWin32OpenGLTextMapper.h"
  #include "vtkWin32TextMapper.h"
#else
#ifdef VTK_USE_OGLR
  #include "vtkXOpenGLTextMapper.h"
#endif
  #include "vtkXTextMapper.h"
#endif

// Creates a new text mapper with Font size 12, bold off, italic off,
// and Arial font
vtkTextMapper::vtkTextMapper()
{
  this->Input = (char*) NULL;
  this->FontSize = 12;
  this->Bold = 0;
  this->Italic = 0;
  this->Shadow = 0;
  this->FontFamily = VTK_ARIAL;
  this->Justification = VTK_TEXT_LEFT;
  this->VerticalJustification = VTK_TEXT_BOTTOM;

  this->TextLines = NULL;
  this->NumberOfLines = 0;
  this->NumberOfLinesAllocated = 0;
  this->LineOffset = 0.0;
  this->LineSpacing = 1.0;
}

vtkTextMapper *vtkTextMapper::New()
{
  // First try to create the object from the vtkObjectFactory
  vtkObject* ret = vtkObjectFactory::CreateInstance("vtkTextMapper");
  if(ret)
    {
    return (vtkTextMapper*)ret;
    }
  // If the factory was unable to create the object, then create it here.

#ifdef _WIN32
#ifndef VTK_USE_NATIVE_IMAGING
  return vtkWin32OpenGLTextMapper::New();
#else
  return vtkWin32TextMapper::New();
#endif
#else
#ifdef VTK_USE_OGLR
#ifndef VTK_USE_NATIVE_IMAGING
  return vtkXOpenGLTextMapper::New();
#else
  return vtkXTextMapper::New();
#endif
#else
  return vtkXTextMapper::New();
#endif
#endif

}

vtkTextMapper::~vtkTextMapper()
{
  if (this->Input)
    {
    delete [] this->Input;
    this->Input = NULL;
    }

  if ( this->TextLines != NULL )
    {
    for (int i=0; i < this->NumberOfLinesAllocated; i++)
      {
      this->TextLines[i]->Delete();
      }
    delete [] this->TextLines;
    }
  
}

//----------------------------------------------------------------------------
void vtkTextMapper::PrintSelf(ostream& os, vtkIndent indent)
{
  vtkMapper2D::PrintSelf(os,indent);

  os << indent << "Line Offset: " << this->LineOffset;
  os << indent << "Line Spacing: " << this->LineSpacing;
  os << indent << "Bold: " << (this->Bold ? "On\n" : "Off\n");
  os << indent << "Italic: " << (this->Italic ? "On\n" : "Off\n");
  os << indent << "Shadow: " << (this->Shadow ? "On\n" : "Off\n");
  os << indent << "FontFamily: " << this->FontFamily << "\n";
  os << indent << "FontSize: " << this->FontSize << "\n";
  os << indent << "Input: " << (this->Input ? this->Input : "(none)") << "\n";
  os << indent << "Justification: ";
  switch (this->Justification)
    {
    case 0: os << "Left  (0)" << endl; break;
    case 1: os << "Centered  (1)" << endl; break;
    case 2: os << "Right  (2)" << endl; break;
    }
  os << indent << "VerticalJustification: ";
  switch (this->VerticalJustification)
    {
    case VTK_TEXT_TOP: os << "Top" << endl; break;
    case VTK_TEXT_CENTERED: os << "Centered" << endl; break;
    case VTK_TEXT_BOTTOM: os << "Bottom" << endl; break;
    }
  
  os << indent << "NumberOfLines: " << this->NumberOfLines << "\n";  
}

int vtkTextMapper::GetWidth(vtkViewport* viewport)
{
  int size[2];
  this->GetSize(viewport, size);
  return size[0];
}

int vtkTextMapper::GetHeight(vtkViewport* viewport)
{
  int size[2];
  this->GetSize(viewport, size);
  return size[1];
}

// Parse the input and create multiple text mappers if multiple lines
// (delimited by \n) are specified.
void vtkTextMapper::SetInput(char *input)
{
  if ( this->Input && input && (!strcmp(this->Input,input))) 
    {
    return;
    }
  if (this->Input) 
    { 
    delete [] this->Input; 
    }  
  if (input)
    {
    this->Input = new char[strlen(input)+1];
    strcpy(this->Input,input);
    }
  else
    {
    this->Input = NULL;
    }
  this->Modified();
  
  int numLines = this->GetNumberOfLines(input);
  
  if ( numLines <= 1) // a line with no "\n"
    {
    this->NumberOfLines = numLines;
    this->LineOffset = 0.0;
    }

  else //multiple lines
    {
    char *line;
    int i;

    if ( numLines > this->NumberOfLinesAllocated )
      {
      // delete old stuff
      if ( this->TextLines )
        {
        for (i=0; i < this->NumberOfLinesAllocated; i++)
          {
          this->TextLines[i]->Delete();
          }
        delete [] this->TextLines;
        }
      
      // allocate new text mappers
      this->NumberOfLinesAllocated = numLines;
      this->TextLines = new vtkTextMapper *[numLines];
      for (i=0; i < numLines; i++)
        {
        this->TextLines[i] = vtkTextMapper::New();
        }
      } //if we need to reallocate
    
    // set the input strings
    this->NumberOfLines = numLines;
    for (i=0; i < this->NumberOfLines; i++)
      {
      line = this->NextLine(input, i);
      this->TextLines[i]->SetInput( line );
      delete [] line;
      }
    }
}

// Determine the number of lines in the Input string (delimited by "\n").
int vtkTextMapper::GetNumberOfLines(char *input)
{
  if ( input == NULL )
    {
    return 0;
    }

  int numLines=1;
  char *ptr = input;

  while ( ptr != NULL )
    {
    if ( (ptr=strstr(ptr,"\n")) != NULL )
      {
      numLines++;
      ptr++; //skip over \n
      }
    }
  
  return numLines;
}

// Get the next \n delimited line. Returns a string that
// must be freed by the calling function.
char *vtkTextMapper::NextLine(char *input, int lineNum)
{
  char *line, *ptr, *ptrEnd;
  int strLen;

  ptr = input;
  for (int i=0; i != lineNum; i++)
    {
    ptr = strstr(ptr,"\n");
    ptr++;
    }
  ptrEnd = strstr(ptr,"\n");
  if ( ptrEnd == NULL )
    {
    ptrEnd = strchr(ptr, '\0');
    }
  
  strLen = ptrEnd - ptr;
  line = new char[strLen+1];
  strncpy(line, ptr, strLen);
  line[strLen] = '\0';

  return line;
}

// Get the size of a multi-line text string
void vtkTextMapper::GetMultiLineSize(vtkViewport* viewport, int size[2])
{
  int i;
  int lineSize[2];
  
  lineSize[0] = lineSize[1] = size[0] = size[1] = 0;
  for ( i=0; i < this->NumberOfLines; i++ )
    {
    this->TextLines[i]->SetItalic(this->Italic);
    this->TextLines[i]->SetBold(this->Bold);
    this->TextLines[i]->SetShadow(this->Shadow);
    this->TextLines[i]->SetFontSize(this->FontSize);
    this->TextLines[i]->SetFontFamily(this->FontFamily);
    this->TextLines[i]->GetSize(viewport, lineSize);
    size[0] = (lineSize[0] > size[0] ? lineSize[0] : size[0]);
    size[1] = (lineSize[1] > size[1] ? lineSize[1] : size[1]);
    }
  
  // add in the line spacing
  this->LineSize = size[1];
  size[1] = this->NumberOfLines* this->LineSpacing * size[1];
}

void vtkTextMapper::RenderOverlayMultipleLines(vtkViewport *viewport, 
                                               vtkActor2D *actor)    
{
  float offset;
  int size[2];
  // make sure LineSize is up to date 
  this->GetMultiLineSize(viewport,size);
  
  switch (this->VerticalJustification)
    {
    case VTK_TEXT_TOP:
      offset = 1.0;
      break;
    case VTK_TEXT_CENTERED:
      offset = -this->NumberOfLines/2.0 + 1;
      break;
    case VTK_TEXT_BOTTOM:
      offset = -(this->NumberOfLines - 1.0);
      break;
    }

  for (int lineNum=0; lineNum < this->NumberOfLines; lineNum++)
    {
    this->TextLines[lineNum]->SetItalic(this->Italic);
    this->TextLines[lineNum]->SetBold(this->Bold);
    this->TextLines[lineNum]->SetShadow(this->Shadow);
    this->TextLines[lineNum]->SetFontSize(this->FontSize);
    this->TextLines[lineNum]->SetFontFamily(this->FontFamily);
    this->TextLines[lineNum]->SetJustification(this->Justification);
    this->TextLines[lineNum]->SetLineOffset(this->LineSize*(lineNum+offset));
    this->TextLines[lineNum]->SetLineSpacing(this->LineSpacing);
    this->TextLines[lineNum]->RenderOverlay(viewport,actor);
    }
}

void vtkTextMapper::RenderOpaqueGeometryMultipleLines(vtkViewport *viewport, 
                                                      vtkActor2D *actor)    
{
  float offset;
  int size[2];
  // make sure LineSize is up to date 
  this->GetMultiLineSize(viewport,size);

  switch (this->VerticalJustification)
    {
    case VTK_TEXT_TOP:
      offset = 1.0;
      break;
    case VTK_TEXT_CENTERED:
      offset = -this->NumberOfLines/2.0 + 1;
      break;
    case VTK_TEXT_BOTTOM:
      offset = -(this->NumberOfLines - 1.0);
      break;
    }

  for (int lineNum=0; lineNum < this->NumberOfLines; lineNum++)
    {
    this->TextLines[lineNum]->SetItalic(this->Italic);
    this->TextLines[lineNum]->SetBold(this->Bold);
    this->TextLines[lineNum]->SetShadow(this->Shadow);
    this->TextLines[lineNum]->SetFontSize(this->FontSize);
    this->TextLines[lineNum]->SetFontFamily(this->FontFamily);
    this->TextLines[lineNum]->SetJustification(this->Justification);
    this->TextLines[lineNum]->SetLineOffset(this->LineSize*(lineNum+offset));
    this->TextLines[lineNum]->SetLineSpacing(this->LineSpacing);
    this->TextLines[lineNum]->RenderOpaqueGeometry(viewport,actor);
    }
}
