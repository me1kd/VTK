/*=========================================================================

Program:   Visualization Toolkit
Module:    vtkWindBladeReader.cxx

Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
All rights reserved.
See Copyright.txt or http://www.kitware.com/Copyright.htm for details.

This software is distributed WITHOUT ANY WARRANTY; without even
the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
PURPOSE.  See the above copyright notice for more information.

=========================================================================*/
#include "vtkWindBladeReader.h"

#include "vtkCallbackCommand.h"
#include "vtkCell.h"
#include "vtkCellData.h"
#include "vtkCellType.h"
#include "vtkDataArraySelection.h"
#include "vtkFloatArray.h"
#include "vtkIdList.h"
#include "vtkInformation.h"
#include "vtkInformationVector.h"
#include "vtkMath.h"
#include "vtkObjectFactory.h"
#include "vtkPointData.h"
#include "vtkPoints.h"
#include "vtkStructuredGrid.h"
#include "vtkUnstructuredGrid.h"
#include "vtkStreamingDemandDrivenPipeline.h"

#include "vtkStringArray.h"
#include "vtkFloatArray.h"
#include "vtkIntArray.h"
#include "vtkPoints.h"
#include "vtkStructuredGrid.h"
#include "vtkUnstructuredGrid.h"
#include "vtkMultiBlockDataSetAlgorithm.h"
#include "vtksys/SystemTools.hxx"

#include <string>
#include <sstream>
#include <iostream>

#include "vtkMultiProcessController.h"
#include "vtkToolkits.h"

#ifdef VTK_USE_MPI

#include "vtkMPI.h"

// copied from MPIImageReader
#ifdef MPI_VERSION
#  if (MPI_VERSION >= 2)
#    define VTK_USE_MPI_IO 1
#  endif
#endif
#if !defined(VTK_USE_MPI_IO) && defined(ROMIO_VERSION)
#  define VTK_USE_MPI_IO 1
#endif
#if !defined(VTK_USE_MPI_IO) && defined(MPI_SEEK_SET)
#  define VTK_USE_MPI_IO 1
#endif

#endif


#ifdef VTK_USE_MPI_IO

// This macro can be wrapped around MPI function calls to easily report errors.
// Reporting errors is more important with file I/O because, unlike network I/O,
// they usually don't terminate the program.
#define MPICall(funcall) \
  { \
  int __my_result = funcall; \
  if (__my_result != MPI_SUCCESS) \
    { \
    char errormsg[MPI_MAX_ERROR_STRING]; \
    int dummy; \
    MPI_Error_string(__my_result, errormsg, &dummy); \
    vtkErrorMacro(<< "Received error when calling" << endl \
                  << #funcall << endl << endl \
                  << errormsg); \
    } \
  }

#endif // VTK_USE_MPI_IO


namespace
{
  const float DRY_AIR_CONSTANT = 287.04;
  const int NUM_PART_SIDES = 4;  // Blade parts rhombus
  const int NUM_BASE_SIDES = 5;  // Base pyramid
  const int LINE_SIZE             = 256;
  const int DIMENSION             = 3;
  const int BYTES_PER_DATA = 4;
  const int SCALAR  = 1;
  const int VECTOR  = 2;
  const int FLOAT   = 1;
  const int INTEGER  = 2;
}


class WindBladeReaderInternal {
public:
#ifndef VTK_USE_MPI_IO
  FILE* FilePtr;   // Open file pointer
#else
  MPI_File FilePtr;
#endif
};

vtkStandardNewMacro(vtkWindBladeReader);

//----------------------------------------------------------------------------
// Constructor for WindBlade Reader
//----------------------------------------------------------------------------
vtkWindBladeReader::vtkWindBladeReader()
{
  this->Filename = NULL;
  this->SetNumberOfInputPorts(0);

  // Set up three output ports for field, blade and ground
  this->SetNumberOfOutputPorts(3);

  // Irregularly spaced grid description for entire problem
  this->Points = vtkPoints::New();
  this->GPoints = vtkPoints::New();
  this->XSpacing = vtkFloatArray::New();
  this->YSpacing = vtkFloatArray::New();
  this->ZSpacing = vtkFloatArray::New();
  this->ZTopographicValues = NULL;

  // Blade geometry
  this->BPoints = vtkPoints::New();
  this->NumberOfBladePoints = 0;
  this->NumberOfBladeCells = 0;

  // Static tower information
  this->NumberOfBladeTowers = 0;
  this->XPosition = vtkFloatArray::New();
  this->YPosition = vtkFloatArray::New();
  this->HubHeight = vtkFloatArray::New();
  this->AngularVeloc = vtkFloatArray::New();
  this->BladeLength  = vtkFloatArray::New();
  this->BladeCount = vtkIntArray::New();

  // Options to include extra files for topography and turbines
  this->UseTopographyFile = 0;
  this->UseTurbineFile = 0;

  // Setup selection callback to modify this object when array selection changes
  this->SelectionObserver = vtkCallbackCommand::New();
  this->SelectionObserver->SetCallback(&vtkWindBladeReader::SelectionCallback);
  this->SelectionObserver->SetClientData(this);

  this->PointDataArraySelection = vtkDataArraySelection::New();
  this->PointDataArraySelection->AddObserver(vtkCommand::ModifiedEvent,
                                             this->SelectionObserver);

  // Variables need to be divided by density
  this->NumberOfTimeSteps = 1;
  this->TimeSteps = NULL;
  this->NumberOfVariables = 0;
  this->DivideVariables = vtkStringArray::New();
  this->DivideVariables->InsertNextValue("UVW");
  this->DivideVariables->InsertNextValue("A-scale turbulence");
  this->DivideVariables->InsertNextValue("B-scale turbulence");
  this->DivideVariables->InsertNextValue("Oxygen");

  this->Data = NULL;

  // Set rank and total number of processors
  this->MPIController = vtkMultiProcessController::GetGlobalController();

  if(this->MPIController)
    {
    this->Rank = this->MPIController->GetLocalProcessId();
    this->TotalRank = this->MPIController->GetNumberOfProcesses();
    }
  else
    {
    this->Rank = 0;
    this->TotalRank = 1;
    }

// make it run on 0 processors (since ParaView doesn't seem to initialize
// when it is single process)
#ifdef VTK_USE_MPI
  if(this->TotalRank == 1)
    {
    int flag;
    MPICall(MPI_Initialized(&flag));
    if(!flag)
      {
      MPICall(MPI_Init(0, 0));
      }
    }
#endif

  this->Internal = new WindBladeReaderInternal();

  // by default don't skip any lines because normal wind files do not
  // have a header
  this->NumberOfLinesToSkip = 0;
}

//----------------------------------------------------------------------------
// Destructor for WindBlade Reader
//----------------------------------------------------------------------------
vtkWindBladeReader::~vtkWindBladeReader()
{
  this->SetFilename(NULL);
  this->PointDataArraySelection->Delete();
  this->DivideVariables->Delete();

  this->XPosition->Delete();
  this->YPosition->Delete();
  this->HubHeight->Delete();
  this->AngularVeloc->Delete();
  this->BladeLength->Delete();
  this->BladeCount->Delete();
  this->XSpacing->Delete();
  this->YSpacing->Delete();
  this->ZSpacing->Delete();

  if (this->ZTopographicValues)
    {
    delete [] this->ZTopographicValues;
    }

  this->Points->Delete();
  this->GPoints->Delete();
  this->BPoints->Delete();

  if (this->Data)
    {
    for (int var = 0; var < this->NumberOfVariables; var++)
      {
      if (this->Data[var])
        {
        this->Data[var]->Delete();
        }
      }
    delete [] this->Data;
    }

  this->SelectionObserver->Delete();

  delete this->Internal;

  // Do not delete the MPIController it is Singleton like and will
  // cleanup itself;
  this->MPIController = NULL;
  if(this->TimeSteps)
    {
    delete [] this->TimeSteps;
    this->TimeSteps = NULL;
    }
}

//----------------------------------------------------------------------------
// Print information about WindBlade Reader
//----------------------------------------------------------------------------
void vtkWindBladeReader::PrintSelf(ostream &os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os, indent);

  os << indent << "Filename: "
     << (this->Filename ? this->Filename : "(NULL)") << endl;

  os << indent << "WholeExent: {" << this->WholeExtent[0] << ", "
     << this->WholeExtent[1] << ", " << this->WholeExtent[2] << ", "
     << this->WholeExtent[3] << ", " << this->WholeExtent[4] << ", "
     << this->WholeExtent[5] << "}" << endl;
  os << indent << "SubExtent: {" << this->SubExtent[0] << ", "
     << this->SubExtent[1] << ", " << this->SubExtent[2] << ", "
     << this->SubExtent[3] << ", " << this->SubExtent[4] << ", "
     << this->SubExtent[5] << "}" << endl;
  os << indent << "VariableArraySelection:" << endl;
  this->PointDataArraySelection->PrintSelf(os, indent.GetNextIndent());
}

//----------------------------------------------------------------------------
int vtkWindBladeReader::ProcessRequest(vtkInformation* reqInfo,
                                       vtkInformationVector** inputVector,
                                       vtkInformationVector* outputVector)
{
  if(reqInfo->Has(vtkDemandDrivenPipeline::REQUEST_DATA_NOT_GENERATED()))
    {
    int port = reqInfo->Get(vtkDemandDrivenPipeline::FROM_OUTPUT_PORT());
    if(port != 0)
      {
      vtkInformation* fieldInfo = outputVector->GetInformationObject(0);
      fieldInfo->Set(vtkDemandDrivenPipeline::DATA_NOT_GENERATED(), 1);
      }
    if(port != 1)
      {
      vtkInformation* bladeInfo = outputVector->GetInformationObject(1);
      bladeInfo->Set(vtkDemandDrivenPipeline::DATA_NOT_GENERATED(), 1);
      }
    if(port != 2)
      {
      vtkInformation* groundInfo = outputVector->GetInformationObject(2);
      groundInfo->Set(vtkDemandDrivenPipeline::DATA_NOT_GENERATED(), 1);
      }
    return 1;
    }
  return this->Superclass::ProcessRequest(reqInfo, inputVector, outputVector);
}

//----------------------------------------------------------------------------
// RequestInformation supplies global meta information
//----------------------------------------------------------------------------
int vtkWindBladeReader::RequestInformation(
  vtkInformation* reqInfo,
  vtkInformationVector** vtkNotUsed(inputVector),
  vtkInformationVector* outputVector)
{
  int port = reqInfo->Get(vtkDemandDrivenPipeline::FROM_OUTPUT_PORT());
  if(port == 0)
    {
    vtkInformation* bladeInfo = outputVector->GetInformationObject(1);
    bladeInfo->Set(vtkDemandDrivenPipeline::REQUEST_DATA_NOT_GENERATED());
    vtkInformation* groundInfo = outputVector->GetInformationObject(2);
    groundInfo->Set(vtkDemandDrivenPipeline::REQUEST_DATA_NOT_GENERATED());
    }
  else if(port == 1)
    {
    vtkInformation* fieldInfo = outputVector->GetInformationObject(0);
    fieldInfo->Set(vtkDemandDrivenPipeline::REQUEST_DATA_NOT_GENERATED());
    vtkInformation* groundInfo = outputVector->GetInformationObject(2);
    groundInfo->Set(vtkDemandDrivenPipeline::REQUEST_DATA_NOT_GENERATED());
    }
  else if(port == 2)
    {
    vtkInformation* fieldInfo = outputVector->GetInformationObject(0);
    fieldInfo->Set(vtkDemandDrivenPipeline::REQUEST_DATA_NOT_GENERATED());
    vtkInformation* bladeInfo = outputVector->GetInformationObject(1);
    bladeInfo->Set(vtkDemandDrivenPipeline::REQUEST_DATA_NOT_GENERATED());
    }
  // Verify that file exists
  if ( !this->Filename )
    {
    vtkErrorMacro("No filename specified");
    return 0;
    }

  // Get ParaView information and output pointers
  vtkInformation* fieldInfo = outputVector->GetInformationObject(0);
  vtkStructuredGrid *field = this->GetFieldOutput();

  vtkInformation* bladeInfo = outputVector->GetInformationObject(1);
  vtkUnstructuredGrid* blade = this->GetBladeOutput();

  vtkInformation* groundInfo = outputVector->GetInformationObject(2);
  vtkStructuredGrid *ground = this->GetGroundOutput();

  // Read global size and variable information from input file one time
  if (this->NumberOfVariables == 0)
    {
    // Read the size of the problem and variables in data set
    if(this->ReadGlobalData() == false)
      {
      return 0;
      }

    // If turbine file exists setup number of cells and points in blades, towers
    if (this->UseTurbineFile == 1)
      {
      this->SetupBladeData();
      }
    // Allocate the ParaView data arrays which will hold the variable data
    this->Data = new vtkFloatArray*[this->NumberOfVariables];
    for (int var = 0; var < this->NumberOfVariables; var++)
      {
      this->Data[var] = vtkFloatArray::New();
      this->Data[var]->SetName(this->VariableName[var].c_str());
      this->PointDataArraySelection->AddArray(this->VariableName[var].c_str());
      }

    // Set up extent information manually for now
    this->WholeExtent[0] = this->WholeExtent[2] = this->WholeExtent[4] = 0;
    this->WholeExtent[1] = this->Dimension[0] - 1;
    this->WholeExtent[3] = this->Dimension[1] - 1;
    this->WholeExtent[5] = this->Dimension[2] - 1;

    // Ground is from level to topography of field, one cell thick
    this->GDimension[0] = this->Dimension[0];
    this->GDimension[1] = this->Dimension[1];
    this->GDimension[2] = 2;

    this->GExtent[0] = this->GExtent[2] = this->GExtent[4] = 0;
    this->GExtent[1] = this->GDimension[0] - 1;
    this->GExtent[3] = this->GDimension[1] - 1;
    this->GExtent[5] = this->GDimension[2] - 1;

    field->SetWholeExtent(this->WholeExtent);
    field->SetDimensions(this->Dimension);
    fieldInfo->Set(vtkStreamingDemandDrivenPipeline::WHOLE_EXTENT(),
                   this->WholeExtent, 6);

    ground->SetWholeExtent(this->GExtent);
    ground->SetDimensions(this->GDimension);
    groundInfo->Set(vtkStreamingDemandDrivenPipeline::WHOLE_EXTENT(),
                   this->GExtent, 6);

    blade->SetWholeExtent(this->WholeExtent);

    // Create the rectilinear coordinate spacing for entire problem
    this->CreateCoordinates();

    // Collect temporal information and attach to both output ports
    if(this->TimeSteps)
      {
      delete [] this->TimeSteps;
      this->TimeSteps = NULL;
      }

    if (this->NumberOfTimeSteps > 0)
      {
      this->TimeSteps = new double[this->NumberOfTimeSteps];

      this->TimeSteps[0] = (double) this->TimeStepFirst;
      for (int step = 1; step < this->NumberOfTimeSteps; step++)
        {
        this->TimeSteps[step] = this->TimeSteps[step-1] +
          (double) this->TimeStepDelta;
        }

      // Tell the pipeline what steps are available
      fieldInfo->Set(vtkStreamingDemandDrivenPipeline::TIME_STEPS(),
                     this->TimeSteps, this->NumberOfTimeSteps);
      bladeInfo->Set(vtkStreamingDemandDrivenPipeline::TIME_STEPS(),
                     this->TimeSteps, this->NumberOfTimeSteps);

      // Range is required to get GUI to show things
      double tRange[2];
      tRange[0] = this->TimeSteps[0];
      tRange[1] = this->TimeSteps[this->NumberOfTimeSteps - 1];
      fieldInfo->Set(vtkStreamingDemandDrivenPipeline::TIME_RANGE(), tRange, 2);
      bladeInfo->Set(vtkStreamingDemandDrivenPipeline::TIME_RANGE(), tRange, 2);
      }
    else
      {
      fieldInfo->Remove(vtkStreamingDemandDrivenPipeline::TIME_STEPS());
      fieldInfo->Set(vtkStreamingDemandDrivenPipeline::TIME_STEPS(),
                     this->TimeSteps, this->NumberOfTimeSteps);
      bladeInfo->Remove(vtkStreamingDemandDrivenPipeline::TIME_STEPS());
      bladeInfo->Set(vtkStreamingDemandDrivenPipeline::TIME_STEPS(),
                     this->TimeSteps, this->NumberOfTimeSteps);
      }
    }
  return 1;
}

//----------------------------------------------------------------------------
// RequestData populates the output object with data for rendering
// Uses three output ports (field, turbine blades, and ground).
// Field data is parallel, blade and ground only on processor 0
//----------------------------------------------------------------------------
int vtkWindBladeReader::RequestData(
  vtkInformation *reqInfo,
  vtkInformationVector **vtkNotUsed(inVector),
  vtkInformationVector *outVector)
{
  int port = reqInfo->Get(vtkDemandDrivenPipeline::FROM_OUTPUT_PORT());

  // Request data for field port
  if (port == 0)
    {
    // Get the information and output pointers
    vtkInformation* fieldInfo = outVector->GetInformationObject(0);
    vtkStructuredGrid *field = GetFieldOutput();

    // Set the extent info for this processor
    fieldInfo->Get(vtkStreamingDemandDrivenPipeline::UPDATE_EXTENT(),
                   this->SubExtent);
    field->SetExtent(this->SubExtent);

    // Set the rectilinear coordinates matching the requested subextents
    // Extents may include ghost cells for filters that require them
    FillCoordinates();
    field->SetPoints(this->Points);

    this->SubDimension[0] = this->SubExtent[1] - this->SubExtent[0] + 1;
    this->SubDimension[1] = this->SubExtent[3] - this->SubExtent[2] + 1;
    this->SubDimension[2] = this->SubExtent[5] - this->SubExtent[4] + 1;

    this->NumberOfTuples = 1;
    for (int dim = 0; dim < DIMENSION; dim++)
      this->NumberOfTuples *= this->SubDimension[dim];

    // Collect the time step requested
    vtkInformationDoubleVectorKey* timeKey =
      static_cast<vtkInformationDoubleVectorKey*>
        (vtkStreamingDemandDrivenPipeline::UPDATE_TIME_STEPS());

    double dTime = 0.0;
    if (fieldInfo->Has(timeKey))
      {
      double* requestedTimeSteps = fieldInfo->Get(timeKey);
      dTime = requestedTimeSteps[0];
      }

    // Actual time for the time step
    field->GetInformation()->Set(vtkDataObject::DATA_TIME_STEPS(), &dTime, 1);

    // Index of the time step to request
    int timeStep = 0;
    while (timeStep < this->NumberOfTimeSteps &&
           this->TimeSteps[timeStep] < dTime)
      {
      timeStep++;
      }

    // Open the data file for time step if needed
    std::ostringstream fileName;
    fileName << this->RootDirectory << "/"
             << this->DataDirectory << "/" << this->DataBaseName
             << this->TimeSteps[timeStep];

#ifndef VTK_USE_MPI_IO
    this->Internal->FilePtr = fopen(fileName.str().c_str(), "rb");
#else
    char* cchar = new char[strlen(fileName.str().c_str()) + 1];
    strcpy(cchar, fileName.str().c_str());
    MPICall(MPI_File_open(MPI_COMM_WORLD, cchar, MPI_MODE_RDONLY, MPI_INFO_NULL, &this->Internal->FilePtr));
    delete [] cchar;
#endif

    if (this->Internal->FilePtr == NULL)
      {
      vtkWarningMacro(<< "Could not open file " << fileName.str());
      }
    // Some variables depend on others, so force their loading
    for (int i = 0; i < this->DivideVariables->GetNumberOfTuples(); i++)
      {
      if (GetPointArrayStatus(this->DivideVariables->GetValue(i)))
        {
        this->SetPointArrayStatus("Density", 1);
        }
      }

    // Examine each file variable to see if it is selected and load
    for (int var = 0; var < this->NumberOfFileVariables; var++)
      {
      if (this->PointDataArraySelection->GetArraySetting(var))
        {
        this->LoadVariableData(var);
        field->GetPointData()->AddArray(this->Data[var]);
        }
      }

    // Divide variables by Density if required
    for (int i = 0; i < this->DivideVariables->GetNumberOfTuples(); i++)
      {
      if (GetPointArrayStatus(this->DivideVariables->GetValue(i)))
        {
        this->DivideByDensity(this->DivideVariables->GetValue(i));
        }
      }

    // Calculate pressure if requested
    if (GetPointArrayStatus("Pressure"))
      {
      int pressure = this->PointDataArraySelection->GetArrayIndex("Pressure");
      int pre = this->PointDataArraySelection->GetArrayIndex("Pressure-Pre");
      int tempg = this->PointDataArraySelection->GetArrayIndex("tempg");
      int density = this->PointDataArraySelection->GetArrayIndex("Density");

      this->CalculatePressure(pressure, pre, tempg, density);
      field->GetPointData()->AddArray(this->Data[pressure]);
      field->GetPointData()->AddArray(this->Data[pressure + 1]);
      }

    // Calculate vorticity if requested
    if (GetPointArrayStatus("Vorticity"))
      {
      int vort = this->PointDataArraySelection->GetArrayIndex("Vorticity");
      int uvw = this->PointDataArraySelection->GetArrayIndex("UVW");
      int density = this->PointDataArraySelection->GetArrayIndex("Density");

      this->CalculateVorticity(vort, uvw, density);
      field->GetPointData()->AddArray(this->Data[vort]);
      }
    // Close file after all data is read
#ifndef VTK_USE_MPI_IO
    fclose(this->Internal->FilePtr);
#else
    MPICall(MPI_File_close(&this->Internal->FilePtr));
#endif

    return 1;
    }

  // Request data is on blade and is displayed only by processor 0
  // Even if the blade is turned off, it must update with time along with field
  if (port == 1)
    {
    if (this->UseTurbineFile == 1 && this->Rank == 0)
      {
      vtkInformation* bladeInfo = outVector->GetInformationObject(1);
      vtkUnstructuredGrid* blade = this->GetBladeOutput();

      // Collect the time step requested
      vtkInformationDoubleVectorKey* timeKey =
        static_cast<vtkInformationDoubleVectorKey*>
          (vtkStreamingDemandDrivenPipeline::UPDATE_TIME_STEPS());

      double dTime = 0.0;
      if (bladeInfo->Has(timeKey))
        {
        double* requestedTimeSteps = bladeInfo->Get(timeKey);
        dTime = requestedTimeSteps[0];
        }

      // Actual time for the time step
      blade->GetInformation()->Set(vtkDataObject::DATA_TIME_STEPS(), &dTime, 1);

      // Index of the time step to request
      int timeStep = 0;
      while (timeStep < this->NumberOfTimeSteps &&
             this->TimeSteps[timeStep] < dTime)
        {
        timeStep++;
        }
      // only rank 0 reads this so we have to be careful with MPI-IO
      this->LoadBladeData(timeStep);
      }
    return 1;
    }

  // Request data in on ground
  if (port == 2)
    {
    vtkInformation* groundInfo = outVector->GetInformationObject(2);
    vtkStructuredGrid *ground = this->GetGroundOutput();

    // Set the extent info for this processor
    groundInfo->Get(vtkStreamingDemandDrivenPipeline::UPDATE_EXTENT(),
                   this->GSubExtent);
    ground->SetExtent(this->GSubExtent);

    // Set the rectilinear coordinates matching the requested subextents
    this->FillGroundCoordinates();
    ground->SetPoints(this->GPoints);
    }

  return 1;
}

//----------------------------------------------------------------------------
// Divide data variable by density for display
//----------------------------------------------------------------------------
void vtkWindBladeReader::DivideByDensity(const char* varName)
{
  int var = this->PointDataArraySelection->GetArrayIndex(varName);
  int density = this->PointDataArraySelection->GetArrayIndex("Density");

  float* varData = this->Data[var]->GetPointer(0);
  float* densityData = this->Data[density]->GetPointer(0);

  int numberOfTuples = this->Data[var]->GetNumberOfTuples();
  int numberOfComponents = this->Data[var]->GetNumberOfComponents();

  int index = 0;
  for (int i = 0; i < numberOfTuples; i++)
    {
    for (int j = 0; j < numberOfComponents; j++)
      {
      varData[index++] /= densityData[i];
      }
    }
}

//----------------------------------------------------------------------------
// Calculate pressure from tempg and density
// Calculate pressure - pre from pressure in first z position
// Requires that all data be present
//----------------------------------------------------------------------------
void vtkWindBladeReader::CalculatePressure(int pressure, int prespre,
                                           int tempg, int density)
{
  // Set the number of components and tuples for the requested data
  this->Data[pressure]->SetNumberOfComponents(1);
  this->Data[pressure]->SetNumberOfTuples(this->NumberOfTuples);
  float* pressureData = this->Data[pressure]->GetPointer(0);

  this->Data[prespre]->SetNumberOfComponents(1);
  this->Data[prespre]->SetNumberOfTuples(this->NumberOfTuples);
  float* prespreData = this->Data[prespre]->GetPointer(0);

  // Read tempg and Density components from file
  float* tempgData = new float[this->BlockSize];
  float* densityData = new float[this->BlockSize];

#ifndef VTK_USE_MPI_IO
  fseek(this->Internal->FilePtr, this->VariableOffset[tempg], SEEK_SET);
  if (fread(tempgData, sizeof(float), this->BlockSize, this->Internal->FilePtr) != this->BlockSize)
    {
    // This is really and error, but for the time being we report a
    // warning
    vtkWarningMacro ("WindBladeReader error reading file: " << this->Filename
                   << " Premature EOF while reading tempgData.");
    }
  fseek(this->Internal->FilePtr, this->VariableOffset[density], SEEK_SET);
  if (fread(densityData, sizeof(float), this->BlockSize, this->Internal->FilePtr) != this->BlockSize)
    {
    // This is really and error, but for the time being we report a
    // warning
    vtkWarningMacro ("WindBladeReader error reading file: " << this->Filename
                   << " Premature EOF while reading densityData.");
    }
#else
  MPI_Status status;
  char native[7] = "native";

  MPICall(MPI_File_set_view(this->Internal->FilePtr, this->VariableOffset[tempg], MPI_BYTE, MPI_BYTE, native, MPI_INFO_NULL));
  MPICall(MPI_File_read_all(this->Internal->FilePtr, tempgData, this->BlockSize, MPI_FLOAT, &status));
  MPICall(MPI_File_set_view(this->Internal->FilePtr, this->VariableOffset[density], MPI_BYTE, MPI_BYTE, native, MPI_INFO_NULL));
  MPICall(MPI_File_read_all(this->Internal->FilePtr, densityData, this->BlockSize, MPI_FLOAT, &status));
#endif

  // Entire block of data is read so to calculate index into that data we
  // must use the entire Dimension and not the SubDimension
  int planeSize = this->Dimension[0] * this->Dimension[1];
  int rowSize = this->Dimension[0];

  // Pressure - pre needs the first XY plane pressure values
  float* firstPressure = new float[this->Dimension[2]];
  for (int k = 0; k < this->Dimension[2]; k++)
    {
    int index = k * planeSize;
    firstPressure[k] = densityData[index] * DRY_AIR_CONSTANT * tempgData[index];
    }

  // Only the requested subextents are stored on this processor
  int pos = 0;
  for (int k = this->SubExtent[4]; k <= this->SubExtent[5]; k++)
    {
    for (int j = this->SubExtent[2]; j <= this->SubExtent[3]; j++)
      {
      for (int i = this->SubExtent[0]; i <= this->SubExtent[1]; i++)
        {
        int index = (k * planeSize) + (j * rowSize) + i;

        // Pressure is function of density and tempg for the same position
        // Pressure - pre is the pressure at a position minus the pressure
        // from the first value in the z plane

        pressureData[pos] = densityData[index] *
                            DRY_AIR_CONSTANT * tempgData[index];
        prespreData[pos] = pressureData[pos] - firstPressure[k];
        pos++;
        }
      }
    }
  delete [] tempgData;
  delete [] densityData;
  delete [] firstPressure;
}

//----------------------------------------------------------------------------
// Calculate vorticity from UVW
// Requires ghost cell information so fetch all data from files for now
//----------------------------------------------------------------------------
void vtkWindBladeReader::CalculateVorticity(int vort, int uvw, int density)
{
  // Set the number of components and tuples for the requested data
  this->Data[vort]->SetNumberOfComponents(1);
  this->Data[vort]->SetNumberOfTuples(this->NumberOfTuples);
  float* vortData = this->Data[vort]->GetPointer(0);

  // Read U and V components (two int block sizes in between)
  float* uData = new float[this->BlockSize];
  float* vData = new float[this->BlockSize];

#ifndef VTK_USE_MPI_IO
  fseek(this->Internal->FilePtr, this->VariableOffset[uvw], SEEK_SET);
  if (fread(uData, sizeof(float), this->BlockSize, this->Internal->FilePtr) != this->BlockSize)
    {
    // This is really and error, but for the time being we report a
    // warning
    vtkWarningMacro ("WindBladeReader error reading file: " << this->Filename
                   << " Premature EOF while reading uData.");
    }
  fseek(this->Internal->FilePtr, (2 * sizeof(int)), SEEK_SET);
  if (fread(vData, sizeof(float), this->BlockSize, this->Internal->FilePtr) != this->BlockSize)
    {
    // This is really and error, but for the time being we report a
    // warning
    vtkWarningMacro ("WindBladeReader error reading file: " << this->Filename
                   << " Premature EOF while reading vData.");
    }
#else
  MPI_Status status;
  char native[7] = "native";

  MPICall(MPI_File_set_view(this->Internal->FilePtr, this->VariableOffset[uvw], MPI_BYTE, MPI_BYTE, native, MPI_INFO_NULL));
  MPICall(MPI_File_read_all(this->Internal->FilePtr, uData, this->BlockSize, MPI_FLOAT, &status));
  MPICall(MPI_File_set_view(this->Internal->FilePtr, (2 * sizeof(int)), MPI_BYTE, MPI_BYTE, native, MPI_INFO_NULL));
  MPICall(MPI_File_read_all(this->Internal->FilePtr, vData, this->BlockSize, MPI_FLOAT, &status));
#endif

  // Read Density component
  float* densityData = new float[this->BlockSize];

#ifndef VTK_USE_MPI_IO
  fseek(this->Internal->FilePtr, this->VariableOffset[density], SEEK_SET);
  if (fread(densityData, sizeof(float), this->BlockSize, this->Internal->FilePtr) != this->BlockSize)
    {
    // This is really and error, but for the time being we report a
    // warning
    vtkWarningMacro ("WindBladeReader error reading file: " << this->Filename
                   << " Premature EOF while reading densityData.");
    }
#else
  MPICall(MPI_File_set_view(this->Internal->FilePtr, this->VariableOffset[density], MPI_BYTE, MPI_BYTE, native, MPI_INFO_NULL));
  MPICall(MPI_File_read_all(this->Internal->FilePtr, densityData, this->BlockSize, MPI_FLOAT, &status));
#endif

  // Divide U and V components by Density
  for (unsigned int i = 0; i < this->BlockSize; i++)
    {
    uData[i] /= densityData[i];
    vData[i] /= densityData[i];
    }

  // Entire block of data is read so to calculate index into that data we
  // must use the entire Dimension and not the SubDimension
  // Only the requested subextents are stored on this processor
  int planeSize = this->Dimension[0] * this->Dimension[1];
  int rowSize = this->Dimension[0];

  // Initialize to 0.0 because edges have no values
  int pos = 0;
  for (int k = this->SubExtent[4]; k <= this->SubExtent[5]; k++)
    {
    for (int j = this->SubExtent[2]; j <= this->SubExtent[3]; j++)
      {
      for (int i = this->SubExtent[0]; i <= this->SubExtent[1]; i++)
        {
        vortData[pos++] = 0.0;
        }
      }
    }

  // For inner positions calculate vorticity
  pos = 0;
  float ddx = this->Step[0];
  float ddy = this->Step[1];

  for (int k = this->SubExtent[4]; k <= this->SubExtent[5]; k++)
    {
    for (int j = this->SubExtent[2]; j <= this->SubExtent[3]; j++)
      {
      for (int i = this->SubExtent[0]; i <= this->SubExtent[1]; i++)
        {
        // Edges are initialized to 0
        if (j == this->SubExtent[2] || j == this->SubExtent[3] ||
            i == this->SubExtent[0] || i == this->SubExtent[1])
          {
          pos++;
          }
        else
          {
          // Vorticity depends on four cells surrounding this cell
          int index_vp = (k * planeSize) + (j * rowSize) + (i + 1);
          int index_vm = (k * planeSize) + (j * rowSize) + (i - 1);
          int index_up = (k * planeSize) + ((j + 1) * rowSize) + i;
          int index_um = (k * planeSize) + ((j - 1) * rowSize) + i;

          vortData[pos++] = ((vData[index_vp] - vData[index_vm]) / ddx) -
            ((uData[index_up] - uData[index_um]) / ddy);
          }
        }
      }
    }
  delete [] uData;
  delete [] vData;
  delete [] densityData;
}

//----------------------------------------------------------------------------
// Load one variable data array of BLOCK structure into ParaView
//----------------------------------------------------------------------------
void vtkWindBladeReader::LoadVariableData(int var)
{
  this->Data[var]->Delete();
  this->Data[var] = vtkFloatArray::New();
  this->Data[var]->SetName(VariableName[var].c_str());

  // Skip to the appropriate variable block and read byte count
  // not used? int byteCount;
#ifndef VTK_USE_MPI_IO
  fseek(this->Internal->FilePtr, this->VariableOffset[var], SEEK_SET);
#else
  char native[7] = "native";
  MPICall(MPI_File_set_view(this->Internal->FilePtr, this->VariableOffset[var], MPI_BYTE, MPI_BYTE, native, MPI_INFO_NULL));
#endif

  // Set the number of components for this variable
  int numberOfComponents = 0;
  if (this->VariableStruct[var] == SCALAR)
    {
    numberOfComponents = 1;
    this->Data[var]->SetNumberOfComponents(numberOfComponents);
    }
  else if (this->VariableStruct[var] == VECTOR)
    {
    numberOfComponents = DIMENSION;
    this->Data[var]->SetNumberOfComponents(numberOfComponents);
    }

  // Set the number of tuples which will allocate all tuples
  this->Data[var]->SetNumberOfTuples(this->NumberOfTuples);

  // For each component of the requested variable load data
  float* block = new float[this->BlockSize];
  float* varData = this->Data[var]->GetPointer(0);

  // Entire block of data is read so to calculate index into that data we
  // must use the entire Dimension and not the SubDimension
  // Only the requested subextents are stored on this processor
  int planeSize = this->Dimension[0] * this->Dimension[1];
  int rowSize = this->Dimension[0];

  for (int comp = 0; comp < numberOfComponents; comp++)
    {
    // Read the block of data
#ifndef VTK_USE_MPI_IO
    size_t cnt;
    if ((cnt = fread(block, sizeof(float), this->BlockSize, this->Internal->FilePtr)) !=
        static_cast<size_t>(this->BlockSize) )
    {
    // This is really and error, but for the time being we report a
    // warning
    vtkWarningMacro ("WindBladeReader error reading file: " << this->Filename
                     << " Premature EOF while reading block of data."
                     << " Expected " << this->BlockSize << " but got " << cnt);
    }
#else
    MPI_Status status;
    MPICall(MPI_File_read_all(this->Internal->FilePtr, block, this->BlockSize, MPI_FLOAT, &status));
#endif

    int pos = comp;
    for (int k = this->SubExtent[4]; k <= this->SubExtent[5]; k++)
      {
      for (int j = this->SubExtent[2]; j <= this->SubExtent[3]; j++)
        {
        for (int i = this->SubExtent[0]; i <= this->SubExtent[1]; i++)
          {
          int index = (k * planeSize) + (j * rowSize) + i;
          varData[pos] = block[index];
          pos += numberOfComponents;
          }
        }
      }

    // Skip closing and opening byte sizes
#ifndef VTK_USE_MPI_IO
    fseek(this->Internal->FilePtr, (2 * sizeof(int)), SEEK_CUR);
#else
    MPICall(MPI_File_seek(this->Internal->FilePtr, (2 * sizeof(int)), MPI_SEEK_CUR));
#endif
  }
  delete [] block;
}

//----------------------------------------------------------------------------
// Load one variable data array of BLOCK structure into ParaView
//----------------------------------------------------------------------------
bool vtkWindBladeReader::ReadGlobalData()
{
  //kwsys_stl::string fileName = this->Filename;
  std::string fileName = this->Filename;
  vtksys::SystemTools::ConvertToUnixSlashes(fileName);

  char inBuf[LINE_SIZE];

#ifndef VTK_USE_MPI_IO
  ifstream inStr(fileName.c_str());
#else
  MPI_File tempFile;
  char native[7] = "native";
  char* cchar = new char[strlen(fileName.c_str()) + 1];
  strcpy(cchar, fileName.c_str());
  MPICall(MPI_File_open(MPI_COMM_WORLD, cchar, MPI_MODE_RDONLY, MPI_INFO_NULL, &tempFile));
  delete [] cchar;

  std::stringstream inStr;
  MPI_Offset i, tempSize;
  MPI_Status status;

  MPICall(MPI_File_get_size(tempFile, &tempSize));
  MPICall(MPI_File_set_view(tempFile, 0, MPI_BYTE, MPI_BYTE, native, MPI_INFO_NULL));

  for(i = 0; i < tempSize; i = i + LINE_SIZE)
    {
    if(i + LINE_SIZE > tempSize)
      {
      MPICall(MPI_File_read_all(tempFile, inBuf, tempSize - i, MPI_BYTE, &status));
      inStr.write(inBuf, tempSize - i);
      }
    else
      {
      MPICall(MPI_File_read_all(tempFile, inBuf, LINE_SIZE, MPI_BYTE, &status));
      inStr.write(inBuf, LINE_SIZE);
      }
    }

  MPICall(MPI_File_close(&tempFile));
#endif

  if (!inStr)
    {
    vtkWarningMacro("Could not open the global .wind file " << fileName);
    }

  std::string::size_type dirPos = std::string(fileName).rfind("/");
  if (dirPos == std::string::npos)
    {
    vtkWarningMacro("Bad input file name " << fileName);
    }
  this->RootDirectory = std::string(fileName).substr(0, dirPos);

  std::string keyword;
  std::string rest;
  std::string headerVersion;

  while (inStr.getline(inBuf, LINE_SIZE))
    {
    if (inBuf[0] != '#' && inStr.gcount() > 1)
      {
      std::string line(inBuf);
      std::string::size_type keyPos = line.find(' ');
      keyword = line.substr(0, keyPos);
      rest = line.substr(keyPos + 1);
      std::istringstream lineStr(rest.c_str());

      // Header information
      if (keyword == "WIND_HEADER_VERSION")
        {
        lineStr >> headerVersion;
        }

      // Topology variables
      else if (keyword == "GRID_SIZE_X")
        {
        lineStr >> this->Dimension[0];
        }
      else if (keyword == "GRID_SIZE_Y")
        {
        lineStr >> this->Dimension[1];
        }
      else if (keyword == "GRID_SIZE_Z")
        {
        lineStr >> this->Dimension[2];
        }
      else if (keyword == "GRID_DELTA_X")
        {
        lineStr >> this->Step[0];
        }
      else if (keyword == "GRID_DELTA_Y")
        {
        lineStr >> this->Step[1];
        }
      else if (keyword == "GRID_DELTA_Z")
        {
        lineStr >> this->Step[2];
        }

      // Geometry variables
      else if (keyword == "USE_TOPOGRAPHY_FILE")
        {
        lineStr >> this->UseTopographyFile;
        }
      else if (keyword == "TOPOGRAPHY_FILE")
        {
        this->TopographyFile = rest;
        }
      else if (keyword == "COMPRESSION")
        {
        lineStr >> this->Compression;
        }
      else if (keyword == "FIT")
        {
        lineStr >> this->Fit;
        }

      // Time variables
      else if (keyword == "TIME_STEP_FIRST")
        {
        lineStr >> this->TimeStepFirst;
        }
      else if (keyword == "TIME_STEP_LAST")
        {
        lineStr >> this->TimeStepLast;
        }
      else if (keyword == "TIME_STEP_DELTA")
        {
        lineStr >> this->TimeStepDelta;
        }

      // Turbine variables
      else if (keyword == "USE_TURBINE_FILE")
        {
        lineStr >> this->UseTurbineFile;
        }
      else if (keyword == "TURBINE_DIRECTORY")
        {
        this->TurbineDirectory = rest;
        }
      else if (keyword == "TURBINE_TOWER")
        {
        this->TurbineTowerName = rest;
        }
      else if (keyword == "TURBINE_BLADE")
        {
        this->TurbineBladeName = rest;
        }

      // Data variables
      else if (keyword == "DATA_DIRECTORY")
        {
        this->DataDirectory = rest;
        }
      else if (keyword == "DATA_BASE_FILENAME")
        {
        this->DataBaseName = rest;
        }
      else if (keyword == "DATA_VARIABLES")
        {
        lineStr >> this->NumberOfFileVariables;
        this->ReadDataVariables(inStr);
        if(this->FindVariableOffsets() == false)
          {
          return false;
          }
        }
      }
    }
  if (this->TimeStepFirst < this->TimeStepLast)
    {
    this->NumberOfTimeSteps = ((this->TimeStepLast - this->TimeStepFirst) /
                                this->TimeStepDelta) + 1;
    }
  return true;
}

//----------------------------------------------------------------------------
//
// Read the field variable information
//
//----------------------------------------------------------------------------
void vtkWindBladeReader::ReadDataVariables(istream& inStr)
{
  char inBuf[LINE_SIZE];
  std::string structType, basicType;

  // Derive Vorticity = f(UVW, Density)
  // Derive Pressure = f(tempg, Density)
  // Derive Pressure - pre = f(Pressure)
  this->NumberOfDerivedVariables = 3;
  this->NumberOfVariables = this->NumberOfFileVariables;
  int totalVariables = this->NumberOfFileVariables +
                       this->NumberOfDerivedVariables;

  this->VariableName = new vtkStdString[totalVariables];
  this->VariableStruct = new int[totalVariables];
  this->VariableCompSize = new int[totalVariables];
  this->VariableBasicType = new int[totalVariables];
  this->VariableByteCount = new int[totalVariables];
  this->VariableOffset = new long int[totalVariables];

  bool hasUVW = false;
  bool hasDensity = false;
  bool hasTempg = false;

  for (int i = 0; i < this->NumberOfFileVariables; i++)
    {
    inStr.getline(inBuf, LINE_SIZE);

    // Variable name
    std::string varLine(inBuf);
    std::string::size_type lastPos = varLine.rfind('"');
    this->VariableName[i] = varLine.substr(1, lastPos-1);

    if (this->VariableName[i] == "UVW")
      {
      hasUVW = true;
      }
    if (this->VariableName[i] == "Density")
      {
      hasDensity = true;
      }
    if (this->VariableName[i] == "tempg")
      {
      hasTempg = true;
      }

    // Structure, number of components, type, number of bytes
    std::string rest = varLine.substr(lastPos+1);
    std::istringstream line(rest);

    line >> structType;
    line >> this->VariableCompSize[i];

    if (structType == "SCALAR")
      {
      this->VariableStruct[i] = SCALAR;
      }
    else if (structType == "VECTOR")
      {
      this->VariableStruct[i] = VECTOR;
      }
    else
      {
      vtkWarningMacro("Error in structure type " << structType);
      }

    line >> basicType;
    line >> this->VariableByteCount[i];

    if (basicType == "FLOAT")
      {
      this->VariableBasicType[i] = FLOAT;
      }
    else if (basicType == "INTEGER")
      {
      this->VariableBasicType[i] = INTEGER;
      }
    else
      {
      vtkWarningMacro("Error in basic type " << basicType);
      }
  }

  // Add any derived variables
  if (hasUVW && hasDensity)
    {
    this->VariableName[this->NumberOfVariables] = "Vorticity";
    this->NumberOfVariables++;
    }
  if (hasTempg && hasDensity)
    {
    this->VariableName[this->NumberOfVariables] = "Pressure";
    this->NumberOfVariables++;
    this->VariableName[this->NumberOfVariables] = "Pressure-Pre";
    this->NumberOfVariables++;
    }
}

//----------------------------------------------------------------------------
//
// Open the first data file and verify that the data is where is should be
// Each data block is enclosed by two ints which record the number of bytes
// Save the file offset for each varible
//
//----------------------------------------------------------------------------
bool vtkWindBladeReader::FindVariableOffsets()
{
  // Open the first data file
  std::ostringstream fileName;
  fileName << this->RootDirectory << "/"
           << this->DataDirectory << "/"
           << this->DataBaseName << this->TimeStepFirst;

#ifndef VTK_USE_MPI_IO
  this->Internal->FilePtr = fopen(fileName.str().c_str(), "rb");
#else
  char* cchar = new char[strlen(fileName.str().c_str()) + 1];
  strcpy(cchar, fileName.str().c_str());
  MPICall(MPI_File_open(MPI_COMM_WORLD, cchar, MPI_MODE_RDONLY, MPI_INFO_NULL, &this->Internal->FilePtr));
  delete [] cchar;
#endif

  if (this->Internal->FilePtr == NULL)
    {
    vtkErrorMacro("Could not open file " << fileName.str());
    return false;
    }

  // Scan file recording offsets which points to the first data value
  int byteCount;

#ifndef VTK_USE_MPI_IO
  if (fread(&byteCount, sizeof(int), 1, this->Internal->FilePtr) != 1)
    {
    // This is really and error, but for the time being we report a
    // warning
    vtkWarningMacro ("WindBladeReader error reading file: " << this->Filename
                   << " Premature EOF while reading byteCount.");
    }
#else
  MPI_Status status;
  char native[7] = "native";

  MPICall(MPI_File_set_view(this->Internal->FilePtr, 0, MPI_BYTE, MPI_BYTE, native, MPI_INFO_NULL));
  MPICall(MPI_File_read_all(this->Internal->FilePtr, &byteCount, 1, MPI_INT, &status));
#endif

  this->BlockSize = byteCount / BYTES_PER_DATA;

  for (int var = 0; var < this->NumberOfFileVariables; var++)
    {
#ifndef VTK_USE_MPI_IO
    this->VariableOffset[var] = ftell(this->Internal->FilePtr);
#else
    MPI_Offset offset;
    MPICall(MPI_File_get_position(this->Internal->FilePtr, &offset));
    this->VariableOffset[var] = offset;
#endif

    // Skip over the SCALAR or VECTOR components for this variable
    int numberOfComponents = 1;
    if (this->VariableStruct[var] == VECTOR)
      {
      numberOfComponents = DIMENSION;
      }

    for (int comp = 0; comp < numberOfComponents; comp++)
      {
      // Skip data plus two integer byte counts
#ifndef VTK_USE_MPI_IO
      fseek(this->Internal->FilePtr, (byteCount+(2 * sizeof(int))), SEEK_CUR);
#else
      MPICall(MPI_File_seek(this->Internal->FilePtr, (byteCount+(2 * sizeof(int))), MPI_SEEK_CUR));
#endif
      }
    }
#ifndef VTK_USE_MPI_IO
  fclose(this->Internal->FilePtr);
#else
  MPICall(MPI_File_close(&this->Internal->FilePtr));
#endif

  return true;
}

//----------------------------------------------------------------------------
// Fill in the rectilinear points for the requested subextents
//----------------------------------------------------------------------------
void vtkWindBladeReader::FillCoordinates()
{
  this->Points->Delete();
  this->Points = vtkPoints::New();

  // If dataset is flat, x and y are constant spacing, z is stretched
  if (this->UseTopographyFile == 0)
    {
    // Save vtkPoints instead of spacing coordinates because topography file
    // requires this to be vtkStructuredGrid and not vtkRectilinearGrid
    for (int k = this->SubExtent[4]; k <= this->SubExtent[5]; k++)
      {
      float z = this->ZSpacing->GetValue(k);
      for (int j = this->SubExtent[2]; j <= this->SubExtent[3]; j++)
        {
        float y = this->YSpacing->GetValue(j);
        for (int i = this->SubExtent[0]; i <= this->SubExtent[1]; i++)
          {
          float x = this->XSpacing->GetValue(i);
          this->Points->InsertNextPoint(x, y, z);
          }
        }
      }
    }

  // If dataset is topographic, x and y are constant spacing
  // Z data is calculated from an x by y topographic data file
  else
    {
    int planeSize = this->Dimension[0] * this->Dimension[1];
    int rowSize = this->Dimension[0];

    for (int k = this->SubExtent[4]; k <= this->SubExtent[5]; k++)
      {
      for (int j = this->SubExtent[2]; j <= this->SubExtent[3]; j++)
        {
        float y = this->YSpacing->GetValue(j);
        for (int i = this->SubExtent[0]; i <= this->SubExtent[1]; i++)
          {
          float x = this->XSpacing->GetValue(i);
          int index = (k * planeSize) + (j * rowSize) + i;
          this->Points->InsertNextPoint(x, y, this->ZTopographicValues[index]);
          }
        }
      }
    }
}

//----------------------------------------------------------------------------
// Fill in the rectilinear points for the requested subextents
//----------------------------------------------------------------------------
void vtkWindBladeReader::FillGroundCoordinates()
{
  this->GPoints->Delete();
  this->GPoints = vtkPoints::New();

  // If dataset is flat, x and y are constant spacing, z is stretched
  if (this->UseTopographyFile == 0)
    {
    // Save vtkPoints instead of spacing coordinates because topography file
    // requires this to be vtkStructuredGrid and not vtkRectilinearGrid
    for (int k = this->GSubExtent[4]; k <= this->GSubExtent[5]; k++)
      {
      float z = this->ZMinValue;
      for (int j = this->GSubExtent[2]; j <= this->GSubExtent[3]; j++)
        {
        float y = this->YSpacing->GetValue(j);
        for (int i = this->GSubExtent[0]; i <= this->GSubExtent[1]; i++)
          {
          float x = this->XSpacing->GetValue(i);
          this->GPoints->InsertNextPoint(x, y, z);
          }
        }
      }
    }

  // If dataset is topographic, x and y are constant spacing
  // Z data is calculated from an x by y topographic data file
  else
    {
    int planeSize = this->GDimension[0] * this->GDimension[1];
    int rowSize = this->GDimension[0];

    for (int k = this->GSubExtent[4]; k <= this->GSubExtent[5]; k++)
      {
      for (int j = this->GSubExtent[2]; j <= this->GSubExtent[3]; j++)
        {
        float y = this->YSpacing->GetValue(j);
        for (int i = this->GSubExtent[0]; i <= this->GSubExtent[1]; i++)
          {
          float x = this->XSpacing->GetValue(i);
          if (k == 0)
            {
            this->GPoints->InsertNextPoint(x, y, this->ZMinValue);
            }
          else
            {
            int indx = ((k - 1) * planeSize) + (j * rowSize) + i;
            this->GPoints->InsertNextPoint(x, y, this->ZTopographicValues[indx]);
            }
          }
        }
      }
    }
}

//----------------------------------------------------------------------------
// Calculate the Points for flat Rectilinear type grid or topographic
// generalized StructuredGrid which is what is being created here
//----------------------------------------------------------------------------
void vtkWindBladeReader::CreateCoordinates()
{
  // If dataset is flat, x and y are constant spacing, z is stretched
  if (this->UseTopographyFile == 0)
    {
    for (int i = 0; i < this->Dimension[0]; i++)
      {
      this->XSpacing->InsertNextValue(i * this->Step[0]);
      }

    for (int j = 0; j < this->Dimension[1]; j++)
      {
      this->YSpacing->InsertNextValue(j * this->Step[1]);
      }

    double maxZ = this->Step[2] * this->Dimension[2];
    for (int k = 0; k < this->Dimension[2]; k++)
      {
      double zcoord = (k * this->Step[2]) + (0.5 * this->Step[2]);
      double zcartesian = GDeform(zcoord, maxZ, 0);
      this->ZSpacing->InsertNextValue(zcartesian);
      }
    }

  // If dataset is topographic, x and y are constant spacing
  // Z data is calculated from an x by y topographic data file
  else
    {
    for (int i = 0; i < this->Dimension[0]; i++)
      {
      this->XSpacing->InsertNextValue(i * this->Step[0]);
      }

    for (int j = 0; j < this->Dimension[1]; j++)
      {
      this->YSpacing->InsertNextValue(j * this->Step[1]);
      }

    this->ZTopographicValues = new float[this->BlockSize];
    this->CreateZTopography(this->ZTopographicValues);

    this->ZMinValue = this->ZTopographicValues[0];
    for (unsigned int k = 0; k < this->BlockSize; k++)
      {
      if (this->ZMinValue > this->ZTopographicValues[k])
        {
        this->ZMinValue = this->ZTopographicValues[k];
        }
      }
    }

  // Set the ground minimum
  if (this->UseTopographyFile == 0 || this->UseTurbineFile == 1)
    {
    this->ZMinValue = -1.0;
    }
}

//----------------------------------------------------------------------------
// Create the z topography from 2D (x,y) elevations and return in zData
//----------------------------------------------------------------------------
void vtkWindBladeReader::CreateZTopography(float* zValues)
{
  // Read the x,y topography data file
  std::ostringstream fileName;
  fileName << this->RootDirectory << "/"
           << this->TopographyFile;

  int blockSize = this->Dimension[0] * this->Dimension[1];
  float* topoData = new float[blockSize];

#ifndef VTK_USE_MPI_IO
  FILE* filePtr = fopen(fileName.str().c_str(), "rb");
#else
  char* cchar = new char[strlen(fileName.str().c_str()) + 1];
  strcpy(cchar, fileName.str().c_str());
  MPICall(MPI_File_open(MPI_COMM_WORLD, cchar, MPI_MODE_RDONLY, MPI_INFO_NULL, &this->Internal->FilePtr));
  delete [] cchar;
#endif

#ifndef VTK_USE_MPI_IO
  fseek(filePtr, BYTES_PER_DATA, SEEK_SET);  // Fortran byte count
  if(fread(topoData, sizeof(float), blockSize, filePtr) != static_cast<size_t>(blockSize) )
    {
    // This is really and error, but for the time being we report a
    // warning
    vtkWarningMacro ("WindBladeReader error reading file: " << this->Filename
                   << " Premature EOF while reading topoData.");
    }
#else
  MPI_Status status;
  char native[7] = "native";

  MPICall(MPI_File_set_view(this->Internal->FilePtr, BYTES_PER_DATA, MPI_BYTE, MPI_BYTE, native, MPI_INFO_NULL));
  MPICall(MPI_File_read_all(this->Internal->FilePtr, topoData, blockSize, MPI_FLOAT, &status));
#endif

  // Initial z coordinate processing
  float* zedge = new float[this->Dimension[2] + 1];
  float* z = new float[this->Dimension[2]];
  float zb;
  int ibctopbot = 1;

  if (ibctopbot == 1)
    {
    for (int k = 0; k <= this->Dimension[2]; k++)
      {
      zedge[k] = k * this->Step[2];
      }
    zb = zedge[this->Dimension[2]];
    for (int k = 0; k < this->Dimension[2]; k++)
      {
      z[k] = k * this->Step[2] + 0.5 * this->Step[2];
      }
    }

  else
    {
    for (int k = 0; k < this->Dimension[2]; k++)
      {
      z[k] = k * this->Step[2];
      }
    zb = z[this->Dimension[2] - 1];
    }

  // Use cubic spline or deformation to calculate z values
  int npoints = 31;
  float* zdata = new float[npoints];
  float* zcoeff = new float[npoints];
  float zcrdata[] = {
        0.0 ,    2.00,    4.00,     6.00,      8.00,
       10.00,   14.00,   18.00,    22.00,     26.00,
       30.00,   34.00,   40.00,    50.00,     70.00,
      100.00,  130.00,  160.00,   200.00,    250.00,
      300.00,  350.00,  450.00,   550.00,    750.00,
      950.00, 1150.00, 1400.00,  1700.00,   2000.00,   2400.00 };

  // No deformation, use spline to define z coefficients
  if (this->Compression == 0.0)
    {
    for (int i = 0; i < npoints; i++)
      {
      zdata[i] = (z[i] * zb) / z[npoints - 1];
      }

    // Call spline with zcoeff being the answer
    this->Spline(zdata, zcrdata, npoints, 99.0e31, 99.0e31, zcoeff);
    }

  // Fill the zValues array depending on compression
  int planeSize = this->Dimension[0] * this->Dimension[1];
  int rowSize = this->Dimension[0];
  int flag = 0;

  for (int k = 0; k < this->Dimension[2]; k++)
    {
    for (int j = 0; j < this->Dimension[1]; j++)
      {
      for (int i = 0; i < this->Dimension[0]; i++)
        {
        int index = (k * planeSize) + (j * rowSize) + i;
        int tIndex = (j * rowSize) + i;

        if (this->Compression == 0.0)
          {
          // Use spline interpolation
          float zinterp;
          this->Splint(zdata, zcrdata, zcoeff, npoints, z[k], &zinterp, flag);
          zValues[index] = zinterp;
          }
        else
          {
          // Use deformation
          zValues[index] = GDeform(z[k], zb, flag) *
            (zb - topoData[tIndex]) / zb + topoData[tIndex];
          }
        }
      }
    }

  delete [] topoData;
  delete [] zedge;
  delete [] z;
  delete [] zdata;
  delete [] zcoeff;

#ifndef VTK_USE_MPI_IO
  fclose(filePtr);
#else
  MPICall(MPI_File_close(&this->Internal->FilePtr));
#endif

}

//----------------------------------------------------------------------------
//
// Stretch the Z coordinate for flat topography
// If flag = 0 compute gdeform(z)
// If flag = 1 compute derivative of gdeform(z)
// Return cubic polynomial fit
//
//----------------------------------------------------------------------------
float vtkWindBladeReader::GDeform(float sigma, float sigmaMax, int flag)
{
  float sigma_2 = sigma * sigma;
  float sigma_3 = sigma_2 * sigma;

  float f = this->Fit;
  float aa1 = this->Compression;

  float aa2 = (f * (1.0 - aa1)) / sigmaMax;
  float aa3 = (1.0 - (aa2 * sigmaMax) - aa1) / (sigmaMax * sigmaMax);

  float zcoord = 0.0;
  if (flag == 0)
    {
    zcoord = (aa3 * sigma_3) + (aa2 * sigma_2) + (aa1 * sigma);
    }
  else if (flag == 1)
    {
    zcoord = (3.0 * aa3 * sigma_2) + (2.0 * aa2 * sigma) + aa1;
    }

  return zcoord;
}

//----------------------------------------------------------------------------
// Cubic spline from Numerical Recipes (altered for zero based arrays)
// Called only once to process entire tabulated function
//
// Given arrays x[0..n-1] and y[0..n-1] containing a tabulated function
// with x0 < x1 < .. < xn-1, and given values yp1 and ypn for the
// first derivative of the interpolating function at points 0 and n-1,
// this routine returns an array y2[0..n-1] that contains the second
// derivatives of the interpolating function.  If yp1 or ypn > e30
// the rougine is signaled to set the corresponding boundary condition
// for a natural spline, with zero second derivative on that boundary.
//----------------------------------------------------------------------------
void vtkWindBladeReader::Spline(
  float* x, float* y,  // arrays
  int n,      // size of arrays
  float yp1, float ypn,  // boundary condition
  float* y2)    // return array
{
  float qn, un;
  float* u = new float[n];

  // Lower boundary condition set to natural spline
  if (yp1 > 0.99e30)
    {
    y2[0] = u[0] = 0.0;
    }

  // Lower boundary condition set to specified first derivative
  else
    {
    y2[0] = -0.5;
    u[0]=(3.0/(x[1]-x[0]))*((y[1]-y[0])/(x[1]-x[0])-yp1);
    }

  // Decomposition loop of tridiagonal algorithm
  for (int i = 1; i < n-1; i++)
    {
    float sig = (x[i] - x[i-1]) / (x[i+1] - x[i-1]);
    float p = sig * y2[i-1] + 2.0;
    y2[i] = (sig - 1.0) / p;
    u[i] = (y[i+1] - y[i]) / (x[i+1] - x[i]) -
      (y[i] - y[i-1]) / (x[i] - x[i-1]);
    u[i] = (6.0 * u[i] / (x[i+1] - x[i-1]) - sig * u[i-1]) / p;
    }

  // Upper boundary condition set to natural spline
  if (ypn > 0.99e30)
    {
    qn = un = 0.0;
    }

  // Upper boundary condition set to specified first derivative
  else
    {
    qn = 0.5;
    un = (3.0 / (x[n-1] - x[n-2])) *
      (ypn - (y[n-1] - y[n-2]) / (x[n-1] -x [n-2]));
    }

  // Back substitution loop of tridiagonal algorithm
  y2[n-1] = (un - qn * u[n-2]) / (qn * y2[n-2] + 1.0);
  for (int k = n - 2; k >= 0; k--)
    {
    y2[k] = y2[k] * y2[k+1] + u[k];
    }

  delete [] u;
}

//----------------------------------------------------------------------------
// Cubic spline interpolation from Numerical Recipes
// Called succeeding times after spline is called once
// Given x, y and y2 arrays from spline return cubic spline interpolated
//----------------------------------------------------------------------------
void vtkWindBladeReader::Splint(
  float* xa, float* ya,   // arrays sent to Spline()
  float* y2a,     // result from Spline()
  int n,      // size of arrays
  float x,    //
  float* y,    // interpolated value
  int kderivative)
{
  // Find the right place in the table by means of bisection
  // Optimal is sequential calls are at random values of x
  int klo = 0;
  int khi = n - 1;
  while (khi - klo > 1)
    {
    int k = (khi + klo) / 2;
    if (xa[k] > x)
      {
      khi = k;
      }
    else
      {
      klo = k;
      }
    }

  float h = xa[khi] - xa[klo];
  float a = (xa[khi] - x) / h;
  float b = (x - xa[klo]) / h;
  if (kderivative == 0)
    {
    *y = a * ya[klo] + b * ya[khi] +
      ((a * a * a - a) * y2a[klo] +
       (b * b * b - b) * y2a[khi]) * (h * h) / 6.0;
    }
  else
    {
    *y = ((ya[khi] - ya[klo]) / h) -
      ((((((3.0 * a * a) - 1.0) * y2a[klo]) -
         (((3.0 * b * b) - 1.0) * y2a[khi])) * h) / 6.0);
    }
}

//----------------------------------------------------------------------------
// Build the turbine towers
// Parse a blade file to set the number of cells and points in blades
//----------------------------------------------------------------------------
void vtkWindBladeReader::SetupBladeData()
{
  // Load the tower information
  std::ostringstream fileName;
  fileName << this->RootDirectory << "/"
           << this->TurbineDirectory << "/"
           << this->TurbineTowerName;
  char inBuf[LINE_SIZE];

#ifndef VTK_USE_MPI_IO
  ifstream inStr(fileName.str().c_str());
#else
  MPI_File tempFile;
  char native[7] = "native";
  char* cchar = new char[strlen(fileName.str().c_str()) + 1];
  strcpy(cchar, fileName.str().c_str());
  MPICall(MPI_File_open(MPI_COMM_WORLD, cchar, MPI_MODE_RDONLY, MPI_INFO_NULL, &tempFile));
  delete [] cchar;

  std::stringstream inStr;
  MPI_Offset i, tempSize;
  MPI_Status status;

  MPICall(MPI_File_get_size(tempFile, &tempSize));
  MPICall(MPI_File_set_view(tempFile, 0, MPI_BYTE, MPI_BYTE, native, MPI_INFO_NULL));

  for(i = 0; i < tempSize; i = i + LINE_SIZE)
    {
    if(i + LINE_SIZE > tempSize)
      {
      MPICall(MPI_File_read_all(tempFile, inBuf, tempSize - i, MPI_BYTE, &status));
      inStr.write(inBuf, tempSize - i);
      }
    else
      {
      MPICall(MPI_File_read_all(tempFile, inBuf, LINE_SIZE, MPI_BYTE, &status));
      inStr.write(inBuf, LINE_SIZE);
      }
    }

  MPICall(MPI_File_close(&tempFile));
#endif

  if (!inStr)
    {
    vtkWarningMacro("Could not open " << fileName.str() << endl);
    }

  // File is ASCII text so read until EOF
  float hubHeight, bladeLength, maxRPM, xPos, yPos, yawAngle;
  float angularVelocity, angleBlade1;
  int numberOfBlades;
  int towerID;
  // all header stuff is here to deal with wind data format changes
  // number of columns tells us if the turbine tower file has at least 13
  // columns. if so then we are dealing with a wind data format that has
  // an extra header in the turbine blade files
  int numColumns = 0;

  // test first line in turbine tower file to see if it has at least 13th column
  // if so then this is indication of "new" format
  if (inStr.getline(inBuf, LINE_SIZE))
    {
    size_t len = strlen(inBuf);
    // number of lines corresponds to number of spaces
    for (size_t j = 0; j < len; j++)
      {
      if (inBuf[j] == ' ')
        {
        numColumns++;
        }
      }
    }
  else
    {
    std::cout << fileName.str().c_str() << " is empty!\n";
    }
  // reset seek position
  inStr.seekg(0, std::ios_base::beg);
  inStr.clear();

  // make sure we skip lines with one character (\n)
  while (inStr.getline(inBuf, LINE_SIZE) && inStr.gcount() > 1)
    {
    std::istringstream line(inBuf);
    line >> towerID >> hubHeight >> bladeLength >> numberOfBlades >> maxRPM;
    line >> xPos >> yPos;
    line >> yawAngle >> angularVelocity >> angleBlade1;

    this->XPosition->InsertNextValue(xPos);
    this->YPosition->InsertNextValue(yPos);
    this->HubHeight->InsertNextValue(hubHeight);
    this->BladeCount->InsertNextValue(numberOfBlades);
    this->BladeLength->InsertNextValue(bladeLength);
    this->AngularVeloc->InsertNextValue(angularVelocity);
    }
  this->NumberOfBladeTowers = XPosition->GetNumberOfTuples();

#ifndef VTK_USE_MPI_IO
  inStr.close();
#endif

  // Calculate the number of cells in unstructured turbine blades
  std::ostringstream fileName2;
  fileName2 << this->RootDirectory << "/"
            << this->TurbineDirectory << "/"
            << this->TurbineBladeName << this->TimeStepFirst;

#ifndef VTK_USE_MPI_IO
  ifstream inStr2(fileName2.str().c_str());
#else
  cchar = new char[strlen(fileName2.str().c_str()) + 1];
  strcpy(cchar, fileName2.str().c_str());
  MPICall(MPI_File_open(MPI_COMM_WORLD, cchar, MPI_MODE_RDONLY, MPI_INFO_NULL, &tempFile));
  delete [] cchar;

  std::stringstream inStr2;

  MPICall(MPI_File_get_size(tempFile, &tempSize));
  MPICall(MPI_File_set_view(tempFile, 0, MPI_BYTE, MPI_BYTE, native, MPI_INFO_NULL));

  for(i = 0; i < tempSize; i = i + LINE_SIZE)
    {
    if(i + LINE_SIZE > tempSize)
      {
      MPICall(MPI_File_read_all(tempFile, inBuf, tempSize - i, MPI_BYTE, &status));
      inStr2.write(inBuf, tempSize - i);
      }
    else
      {
      MPICall(MPI_File_read_all(tempFile, inBuf, LINE_SIZE, MPI_BYTE, &status));
      inStr2.write(inBuf, LINE_SIZE);
      }
    }

  MPICall(MPI_File_close(&tempFile));
#endif

  if (!inStr2)
    {
    vtkWarningMacro("Could not open blade file: " << fileName2.str().c_str() <<
                    " to calculate blade cells.");
    for (int j = this->TimeStepFirst + this->TimeStepDelta; j <= this->TimeStepLast;
         j += this->TimeStepDelta)
      {
      std::ostringstream fileName3;
      fileName3 << this->RootDirectory << "/"
                << this->TurbineDirectory << "/"
                << this->TurbineBladeName << j;
      //std::cout << "Trying " << fileName3.str().c_str() << "...";

#ifndef VTK_USE_MPI_IO
      inStr2.open(fileName3.str().c_str());
#else
      cchar = new char[strlen(fileName3.str().c_str()) + 1];
      strcpy(cchar, fileName3.str().c_str());
      MPICall(MPI_File_open(MPI_COMM_WORLD, cchar, MPI_MODE_RDONLY, MPI_INFO_NULL, &tempFile));
      delete [] cchar;

      inStr2.clear();
      inStr2.str("");

      MPICall(MPI_File_get_size(tempFile, &tempSize));
      MPICall(MPI_File_set_view(tempFile, 0, MPI_BYTE, MPI_BYTE, native, MPI_INFO_NULL));

      for(i = 0; i < tempSize; i = i + LINE_SIZE)
        {
        if(i + LINE_SIZE > tempSize)
          {
          MPICall(MPI_File_read_all(tempFile, inBuf, tempSize - i, MPI_BYTE, &status));
          inStr2.write(inBuf, tempSize - i);
          }
        else
          {
          MPICall(MPI_File_read_all(tempFile, inBuf, LINE_SIZE, MPI_BYTE, &status));
          inStr2.write(inBuf, LINE_SIZE);
          }
        }

      MPICall(MPI_File_close(&tempFile));
#endif

      if(inStr2.good())
        {
        vtkWarningMacro("Success with " << fileName3.str());
        break;
        }
      else
        {
        vtkWarningMacro("Failure with " << fileName3.str());
        }
      }
    }

  this->NumberOfBladeCells = 0;
  // if we have at least 13 columns, then this is the new format with a header in the
  // turbine blade file
  if (numColumns >= 13 && inStr2)
    {
    int linesSkipped = 0;
    // each blade tower tries to split the columns such that there are
    // five items per line in header, so skip those lines
    this->NumberOfLinesToSkip = this->NumberOfBladeTowers*(int)ceil(numColumns/5.0);
    // now skip the first few lines based on header, if that applies
    while(inStr2.getline(inBuf, LINE_SIZE) &&
          linesSkipped < this->NumberOfLinesToSkip-1)
      {
      linesSkipped++;
      }
    }
  while (inStr2.getline(inBuf, LINE_SIZE))
    this->NumberOfBladeCells++;
#ifndef VTK_USE_MPI_IO
  inStr2.close();
#endif
  this->NumberOfBladePoints = this->NumberOfBladeCells * NUM_PART_SIDES;

  // Points and cells needed for constant towers
  this->NumberOfBladePoints += this->NumberOfBladeTowers * NUM_BASE_SIDES;
  this->NumberOfBladeCells += this->NumberOfBladeTowers;
}

//----------------------------------------------------------------------------
// Build the turbine blades
//----------------------------------------------------------------------------
void vtkWindBladeReader::LoadBladeData(int timeStep)
{
  this->BPoints->Delete();
  this->BPoints = vtkPoints::New();

  // Open the file for this time step
  std::ostringstream fileName;
  fileName << this->RootDirectory << "/"
           << this->TurbineDirectory << "/"
           << this->TurbineBladeName
           << this->TimeSteps[timeStep];
  char inBuf[LINE_SIZE];

#ifndef VTK_USE_MPI_IO
  ifstream inStr(fileName.str().c_str());
#else
  // only rank 0 reads this so we have to be careful
  MPI_File tempFile;
  char native[7] = "native";
  char* cchar = new char[strlen(fileName.str().c_str()) + 1];
  strcpy(cchar, fileName.str().c_str());
  // here only rank 0 opens it : MPI_COMM_SELF
  MPICall(MPI_File_open(MPI_COMM_SELF, cchar, MPI_MODE_RDONLY, MPI_INFO_NULL, &tempFile));
  delete [] cchar;

  std::stringstream inStr;
  MPI_Offset i, tempSize;
  MPI_Status status;

  MPICall(MPI_File_get_size(tempFile, &tempSize));
  MPICall(MPI_File_set_view(tempFile, 0, MPI_BYTE, MPI_BYTE, native, MPI_INFO_NULL));

  for(i = 0; i < tempSize; i = i + LINE_SIZE)
    {
    if(i + LINE_SIZE > tempSize)
      {
      MPICall(MPI_File_read(tempFile, inBuf, tempSize - i, MPI_BYTE, &status));
      inStr.write(inBuf, tempSize - i);
      }
    else
      {
      MPICall(MPI_File_read(tempFile, inBuf, LINE_SIZE, MPI_BYTE, &status));
      inStr.write(inBuf, LINE_SIZE);
      }
    }

  MPICall(MPI_File_close(&tempFile));
#endif
  /*
  if (this->Rank == 0)
    cout << "Load file " << fileName.str() << endl;
  */

  // Allocate space for points and cells
  this->BPoints->Allocate(this->NumberOfBladePoints, this->NumberOfBladePoints);
  vtkUnstructuredGrid* blade = GetBladeOutput();
  blade->Allocate(this->NumberOfBladeCells, this->NumberOfBladeCells);
  blade->SetPoints(this->BPoints);

  // Allocate space for data
  vtkFloatArray* force1 = vtkFloatArray::New();
  force1->SetName("Force 1");
  force1->SetNumberOfTuples(this->NumberOfBladeCells);
  force1->SetNumberOfComponents(1);
  blade->GetCellData()->AddArray(force1);
  float* aBlock = force1->GetPointer(0);

  vtkFloatArray* force2 = vtkFloatArray::New();
  force2->SetName("Force 2");
  force2->SetNumberOfTuples(this->NumberOfBladeCells);
  force2->SetNumberOfComponents(1);
  blade->GetCellData()->AddArray(force2);
  float* bBlock = force2->GetPointer(0);

  vtkFloatArray* bladeComp = vtkFloatArray::New();
  bladeComp->SetName("Blade Component");
  bladeComp->SetNumberOfTuples(this->NumberOfBladeCells);
  bladeComp->SetNumberOfComponents(1);
  blade->GetCellData()->AddArray(bladeComp);
  float* compBlock = bladeComp->GetPointer(0);

  // blade velocity at point is angular velocity X dist from hub
  vtkFloatArray* bladeVeloc = vtkFloatArray::New();
  bladeVeloc->SetName("Blade Velocity");
  bladeVeloc->SetNumberOfComponents(1);
  bladeVeloc->SetNumberOfTuples(this->NumberOfBladePoints);
  blade->GetPointData()->AddArray(bladeVeloc);
  
  vtkFloatArray* bladeAzimUVW = vtkFloatArray::New();
  bladeAzimUVW->SetName("Blade Azimuthal UVW");
  bladeAzimUVW->SetNumberOfComponents(3);
  bladeAzimUVW->SetNumberOfTuples(this->NumberOfBladePoints);
  blade->GetPointData()->AddArray(bladeAzimUVW);
  
  vtkFloatArray* bladeAxialUVW = vtkFloatArray::New();
  bladeAxialUVW->SetName("Blade Axial UVW");
  bladeAxialUVW->SetNumberOfComponents(3);
  bladeAxialUVW->SetNumberOfTuples(this->NumberOfBladePoints);
  blade->GetPointData()->AddArray(bladeAxialUVW);
  
  vtkFloatArray* bladeDragUVW = vtkFloatArray::New();
  bladeDragUVW->SetName("Blade Drag UVW");
  bladeDragUVW->SetNumberOfComponents(3);
  bladeDragUVW->SetNumberOfTuples(this->NumberOfBladePoints);
  blade->GetPointData()->AddArray(bladeDragUVW);
  
  vtkFloatArray* bladeLiftUVW = vtkFloatArray::New();
  bladeLiftUVW->SetName("Blade Lift UVW");
  bladeLiftUVW->SetNumberOfComponents(3);
  bladeLiftUVW->SetNumberOfTuples(this->NumberOfBladePoints);
  blade->GetPointData()->AddArray(bladeLiftUVW);

  // File is ASCII text so read until EOF
  int index = 0;
  int indx = 0;
  int firstPoint;
  int turbineID, lastTurbineID = 1, bladeID, partID;
  float x, y, z;
  vtkIdType cell[NUM_BASE_SIDES];

  int linesRead = 0;
  float bladeAzimUVWVec[3]  = { 0.0, 0.0, 0.0 },
        bladeAxialUVWVec[3] = { 1.0, 0.0, 0.0 }, 
        bladeDragUVWVec[3]  = { 0.0, 0.0, 0.0 },
        bladeLiftUVWVec[3]  = { 0.0, 0.0, 0.0 };
  int   turbineHeaderStartIndex = 0, turbineIDHeader = 0;
  float hubPnt[3];
  // blade component id is component count + blade ID
  // component count is basically the number of blades thus far
  int   bladeComponentCount = 0;
  while (inStr.getline(inBuf, LINE_SIZE))
    {
    // if header exists, grab necessary items from it
    linesRead++;
    std::istringstream line(inBuf);
    // if we are still in header...
    if (linesRead <= this->NumberOfLinesToSkip)
      {
      // identify beginning of header information per
      // turbine
      if (!(linesRead % 3))
        {
        turbineHeaderStartIndex = linesRead;
        turbineIDHeader++;
        }
      // second line has blade length
      if ((linesRead - turbineHeaderStartIndex) == 1)
        {
        // skip data items to get to necessary field
        float parsedItem = 0.0f;
        for (int j = 0; j < 3; j++)
          {
          line >> parsedItem;
          }
        this->BladeLength->SetTuple1(turbineIDHeader, parsedItem);
        }
      // third line has angular velocity
      if ((linesRead - turbineHeaderStartIndex) == 2)
        {
        // skip items to get to angular velocity
        float parsedItem = 0.0f;
        for (int j = 0; j < 4; j++)
          {
          line >> parsedItem;
          }
        this->AngularVeloc->SetTuple1(turbineIDHeader, parsedItem);
        }
      continue;
      }

    line >> turbineID >> bladeID >> partID;
    // if we have encountered a new turbine, make sure blade component
    // count is updated. this ensures that the component id of future blades
    // start from a valid index
    if (turbineID != lastTurbineID)
      { 
      bladeComponentCount = (int)compBlock[indx-1];
      lastTurbineID       = turbineID;
      }
    // turbineID start from 1, but float array starts from 0
    float angularVelocity = this->AngularVeloc->GetTuple1(turbineID-1);
    // where blades connect to
    hubPnt[0] = this->XPosition->GetValue(turbineID-1);
    hubPnt[1] = this->YPosition->GetValue(turbineID-1);
    hubPnt[2] = this->HubHeight->GetValue(turbineID-1);

    firstPoint = index;

    for (int side = 0; side < NUM_PART_SIDES; side++)
      {
      line >> x >> y >> z;
      this->BPoints->InsertNextPoint(x, y, z);
      // distance to hub-blade connect point
      float bladePnt[3] = {x, y, z};
      float dist = vtkMath::Distance2BetweenPoints(hubPnt, bladePnt);
      float radialVeloc = angularVelocity*sqrt(dist);
      bladeVeloc->InsertTuple1(firstPoint + side, radialVeloc);
      }
    
    // compute blade's various drag/lift/etc vectors; 
    // re-use for all cross-sections per blade. 
    int sectionNum = (firstPoint/NUM_PART_SIDES)%100;
    if (sectionNum == 0)
      {
      vtkIdType numBPnts = this->BPoints->GetNumberOfPoints();
      // create two vectors to calculate cross-product, to make
      // azimuthal
      double pntD[3], pntC[3];
      // points from trailing edge
      this->BPoints->GetPoint(numBPnts-1, pntD);
      this->BPoints->GetPoint(numBPnts-2, pntC);
      float vec1[3] = { static_cast<float>(pntD[0] - pntC[0]),
                        static_cast<float>(pntD[1] - pntC[1]),
                        static_cast<float>(pntD[2] - pntC[2]) };
      float vec2[3] = { 1.0, 0.0, 0.0};
      vtkMath::Cross(vec2, vec1, bladeAzimUVWVec);
      vtkMath::Normalize(bladeAzimUVWVec);
        
      // for drag, we require "chord line," requires one point
      // from leading edge
      double pntA[3];
      this->BPoints->GetPoint(numBPnts-4, pntA);
      // chord line
      bladeDragUVWVec[0] = pntC[0] - pntA[0];
      bladeDragUVWVec[1] = pntC[1] - pntA[1];  
      bladeDragUVWVec[2] = pntC[2] - pntA[2];
      vtkMath::Normalize(bladeDragUVWVec);
      vtkMath::Cross(bladeDragUVWVec, vec1, bladeLiftUVWVec);
      vtkMath::Normalize(bladeLiftUVWVec);
      }
      
    for (int side = 0; side < NUM_PART_SIDES; side++)
      {
      bladeAzimUVW->InsertTuple(firstPoint  + side, bladeAzimUVWVec); 
      bladeAxialUVW->InsertTuple(firstPoint + side, bladeAxialUVWVec);
      bladeDragUVW->InsertTuple(firstPoint  + side, bladeDragUVWVec);
      bladeLiftUVW->InsertTuple(firstPoint  + side, bladeLiftUVWVec);
      }

    // Polygon points are leading edge then trailing edge so points are 0-1-3-2
    // i.e. if "-----" denotes the edge, then the order of cross-section is:
    // 3 ----- 2 (trailing)
    // 1 ----- 0 (leading)
    cell[0] = firstPoint;
    cell[1] = firstPoint + 1;
    cell[2] = firstPoint + 3;
    cell[3] = firstPoint + 2;
    index += NUM_PART_SIDES;
    blade->InsertNextCell(VTK_POLYGON, NUM_PART_SIDES, cell);

    line >> aBlock[indx] >> bBlock[indx];
    compBlock[indx] = bladeID + bladeComponentCount;
    indx++;
  }

  // Add the towers to the geometry
  for (int j = 0; j < this->NumberOfBladeTowers; j++)
    {
    x = this->XPosition->GetValue(j);
    y = this->YPosition->GetValue(j);
    z = this->HubHeight->GetValue(j);

    this->BPoints->InsertNextPoint(x-2, y-2, 0.0);
    this->BPoints->InsertNextPoint(x+2, y-2, 0.0);
    this->BPoints->InsertNextPoint(x+2, y+2, 0.0);
    this->BPoints->InsertNextPoint(x-2, y+2, 0.0);
    this->BPoints->InsertNextPoint(x, y, z);
    firstPoint = index;
    cell[0] = firstPoint;
    cell[1] = firstPoint + 1;
    cell[2] = firstPoint + 2;
    cell[3] = firstPoint + 3;
    cell[4] = firstPoint + 4;
    
    for (int k = 0; k < 5; k++)
    {
      bladeVeloc->InsertTuple1(k + firstPoint, 0.0);  
      bladeAzimUVW->InsertTuple3(k + firstPoint, 0.0, 0.0, 0.0);
      bladeAxialUVW->InsertTuple3(k + firstPoint, 0.0, 0.0, 0.0);
      bladeDragUVW->InsertTuple3(k + firstPoint, 0.0, 0.0, 0.0);
      bladeLiftUVW->InsertTuple3(k + firstPoint, 0.0, 0.0, 0.0);
    }
      
    index += NUM_BASE_SIDES;
    blade->InsertNextCell(VTK_PYRAMID, NUM_BASE_SIDES, cell);

    aBlock[indx]    = 0.0;
    bBlock[indx]    = 0.0;
    compBlock[indx] = 0.0;
    indx++;
    }

  force1->Delete();
  force2->Delete();
  bladeComp->Delete();
  bladeVeloc->Delete();
  bladeAzimUVW->Delete();
  bladeAxialUVW->Delete();
  bladeDragUVW->Delete();
  bladeLiftUVW->Delete();
}

//----------------------------------------------------------------------------
void vtkWindBladeReader::SelectionCallback(
  vtkObject*, unsigned long vtkNotUsed(eventid), void* clientdata, void* vtkNotUsed(calldata))
{
  static_cast<vtkWindBladeReader*>(clientdata)->Modified();
}

//----------------------------------------------------------------------------
int vtkWindBladeReader::GetNumberOfPointArrays()
{
  return this->PointDataArraySelection->GetNumberOfArrays();
}

//----------------------------------------------------------------------------
void vtkWindBladeReader::EnableAllPointArrays()
{
  this->PointDataArraySelection->EnableAllArrays();
}

//----------------------------------------------------------------------------
void vtkWindBladeReader::DisableAllPointArrays()
{
  this->PointDataArraySelection->DisableAllArrays();
}

//----------------------------------------------------------------------------
const char* vtkWindBladeReader::GetPointArrayName(int index)
{
  return this->VariableName[index].c_str();
}

//----------------------------------------------------------------------------
int vtkWindBladeReader::GetPointArrayStatus(const char* name)
{
  return this->PointDataArraySelection->ArrayIsEnabled(name);
}

//----------------------------------------------------------------------------
void vtkWindBladeReader::SetPointArrayStatus(const char* name, int status)
{
  if (status)
    {
    this->PointDataArraySelection->EnableArray(name);
    }
  else
    {
    this->PointDataArraySelection->DisableArray(name);
    }
}

//----------------------------------------------------------------------------
vtkStructuredGrid* vtkWindBladeReader::GetFieldOutput()
{
  return vtkStructuredGrid::SafeDownCast(
    this->GetExecutive()->GetOutputData(0));
}

//----------------------------------------------------------------------------
vtkUnstructuredGrid *vtkWindBladeReader::GetBladeOutput()
{
  if (this->GetNumberOfOutputPorts() < 2)
    {
    return NULL;
    }
  return vtkUnstructuredGrid::SafeDownCast(
    this->GetExecutive()->GetOutputData(1));
}

//----------------------------------------------------------------------------
vtkStructuredGrid *vtkWindBladeReader::GetGroundOutput()
{
  if (this->GetNumberOfOutputPorts() < 3)
    {
    return NULL;
    }
  return vtkStructuredGrid::SafeDownCast(
    this->GetExecutive()->GetOutputData(2));
}

//----------------------------------------------------------------------------
int vtkWindBladeReader::FillOutputPortInformation(int port,
                                                  vtkInformation* info)
{
  // Field data
  if (port == 0)
    {
    return this->Superclass::FillOutputPortInformation(port, info);
    }
  // Blade data
  if (port == 1)
    {
    info->Set(vtkDataObject::DATA_TYPE_NAME(), "vtkUnstructuredGrid");
    }
  // Ground data for topology
  if (port == 2)
    {
    info->Set(vtkDataObject::DATA_TYPE_NAME(), "vtkStructuredGrid");
    }
  return 1;
}
