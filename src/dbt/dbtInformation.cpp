/*
 * dbtInformation.cpp
 *
 *  Created on: 14 janv. 2019
 *      Author: simon
 */

#include <cstring>
#include <dbt/dbtPlateform.h>
#include <dbt/insertions.h>
#include <dbt/profiling.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <lib/endianness.h>
#include <simulator/emptySimulator.h>

#include <transformation/buildControlFlow.h>
#include <transformation/buildTraces.h>
#include <transformation/irGenerator.h>
#include <transformation/memoryDisambiguation.h>
#include <transformation/optimizeBasicBlock.h>
#include <transformation/reconfigureVLIW.h>
#include <transformation/rescheduleProcedure.h>

#include <isa/riscvISA.h>
#include <isa/vexISA.h>

#include <lib/config.h>
#include <lib/log.h>

#include <transformation/firstPassTranslation.h>

#include <dbt/dbtInformation.h>
#include <vector>

#ifndef __NIOS
#include <lib/elfFile.h>
#else
#include <system.h>
#endif
#include <assert.h>
#include <fstream>
#include <iostream>
#include <isa/irISA.h>
extern "C"

/****************************************************************************************************************************/

#define IT_NB_SET 8
#define IT_NB_WAY 8

#define COST_OPT_1 10
#define COST_OPT_2 200

#define MAX_IT_COUNTER 30
#define THRESHOLD_OPTI1 3
#define THRESHOLD_OPTI2 7
    /****************************************************************************************************************************/

typedef struct BlockInformation {
  IRBlock* block;
  int scheduleSizeOpt0 = -1;
  int scheduleSizeOpt1 = -1;
  int scheduleSizeOpt2 = -1;
  int nbChargement     = 0;
  int nbOpti1          = 0;
  int nbOpti2          = 0;
  int nbExecution      = 0;
} BlockInformation;

struct entryInTranslationCache {
  IRBlock* block;
  IRProcedure* procedure;
  bool isBlock;
  unsigned int size;
  unsigned int cost;
};

struct indirectionTableEntry {
  uint64_t address;
  uint8_t counter;
  bool isInTC;
  char optLevel;
  int lastCycleTouch;     // Needed to set a LRU policy
  unsigned int sizeInTC;  // Reprensent the size of the binaries in the TC (counter as the number of instruction)
  uint64_t timeAvailable; // Reprensent the timestamp where the optimization is finished. If current cycle number is
                          // lower we take the optimization level just below
  unsigned int costOfBinaries; // Cost function that represent the time spent on the binaries.
};

/****************************************************************************************************************************/

BlockInformation* blockInfo;
char* execPath;

DBTPlateform *platform, *nonOptPlatform;
IRApplication* application;
unsigned int placeCode;
int greatestAddr;

struct indirectionTableEntry indirectionTable[IT_NB_WAY][IT_NB_SET];
uint64_t nextAvailabilityDBTProc = 0;
int sizeLeftInTC                 = 8192;

std::vector<struct entryInTranslationCache>* translationCacheContent = NULL;
int globalBinarySize;

bool isExploreOpts = false;

char* filenameMetric;
uint64_t nb_cycle_for_eval;
unsigned int nb_max_counter = 0, nb_tc_too_small = 0, nb_tc_too_full = 0,
              nb_try_proc_in_tc = 0, nb_proc_in_tc = 0;

unsigned int SIZE_TC = 0;
/****************************************************************************************************************************/
// Definition of internal function that are not visible from outside

bool allocateInTranslationCache(int size, IRProcedure* procedure, IRBlock* block);
int evalFunction(struct entryInTranslationCache a, int* way, int* set);

/****************************************************************************************************************************/

int translateOneSection(DBTPlateform& dbtPlateform, unsigned int placeCode, int sourceStartAddress,
                        int sectionStartAddress, int sectionEndAddress)
{
  unsigned int size = (sectionEndAddress - sectionStartAddress) >> 2;
  placeCode         = firstPassTranslator(&dbtPlateform, size, sourceStartAddress, sectionStartAddress, placeCode);

  return placeCode;
}

void readSourceBinaries(char* path, unsigned char*& code, unsigned int& addressStart, unsigned int& size,
                        unsigned int& pcStart, DBTPlateform* platform)
{

  // We initialize size at 0
  size                      = 0;
  unsigned char* resultCode = NULL;
  unsigned char* buffer;
  addressStart = 0xfffffff;

  // We open the elf file and search for the section that is of interest for us
  ElfFile elfFile(path);

  for (unsigned int sectionCounter = 0; sectionCounter < elfFile.sectionTable->size(); sectionCounter++) {
    ElfSection* section = elfFile.sectionTable->at(sectionCounter);

    // The section we are looking for is called .text
    if (!section->getName().compare(".text") || !section->getName().compare("__libc_freeres_fn")) {
      if (resultCode == NULL) {
        resultCode = (unsigned char*)malloc(section->size * sizeof(char));
      } else {
        resultCode = (unsigned char*)realloc(resultCode, (size * 4 + section->size) * sizeof(char));
      }
      buffer = section->getSectionCode(); // 0x3c
      memcpy(&resultCode[size * 4], buffer, section->size * sizeof(char));
      size += section->size / 4 - 0;

      free(buffer);
      if (addressStart > section->address) {
        addressStart = section->address;
      }
    }
  }

  code = resultCode;

  if (size > MEMORY_SIZE) {
    free(platform->vliwBinaries);
    free(platform->mipsBinaries);
    free(platform->blockBoundaries);
    platform->vliwBinaries    = (unsigned int*)malloc(4 * size * 2 * sizeof(unsigned int));
    platform->mipsBinaries    = (unsigned int*)malloc(4 * size * 2 * sizeof(unsigned int));
    platform->blockBoundaries = (unsigned char*)malloc(size * 2 * sizeof(unsigned char));
  }
}

int contentTranslationCache()
{
  int size = 0;
  for (int oneElement = 0; oneElement < translationCacheContent->size(); oneElement++) {
    struct entryInTranslationCache entry = translationCacheContent->at(oneElement);
    size += entry.size;
  }

  return size;
}

void updateOpt2BlockSize(IRBlock* block)
{
  // No need to compute if we already have a value
  if (block->sizeOpt2 != -1)
    return;

  blockInfo[block->sourceStartAddress].scheduleSizeOpt2 = block->vliwEndAddress - block->vliwStartAddress;
  block->sizeOpt2                                       = block->vliwEndAddress - block->vliwStartAddress;

  if (block->blockState == IRBLOCK_UNROLLED && block->unrollingFactor != 0) {
    blockInfo[block->sourceStartAddress].scheduleSizeOpt2 =
        blockInfo[block->sourceStartAddress].scheduleSizeOpt2 / block->unrollingFactor;
    block->sizeOpt2 = blockInfo[block->sourceStartAddress].scheduleSizeOpt2;
  }

  int currentjumpId = 0;

  int nbBlockAdded = 0;
  if (block->blockState == IRBLOCK_TRACE && block->nbMergedBlocks != 0) {

    for (int oneSourceCycle = block->sourceStartAddress + 1; oneSourceCycle < block->sourceEndAddress;
         oneSourceCycle++) {
      // TODO: use block iterator ?
      if (application->getBlock(oneSourceCycle) != NULL) {
        IRBlock* otherBlock  = application->getBlock(oneSourceCycle);
        otherBlock->sizeOpt2 = block->sizeOpt2 / (block->nbMergedBlocks + 1);

        //
        // if (block->nbJumps == 0) {
        //   otherBlock->sizeOpt2 = block->sizeOpt2 / block->nbMergedBlocks;
        // } else if (currentjumpId >= block->nbJumps) {
        //   // blockInfo[oneSourceCycle].scheduleSizeOpt2 = (block->vliwEndAddress - block->vliwStartAddress) / 2;
        //   otherBlock->sizeOpt2 = block->vliwEndAddress - block->jumpPlaces[block->nbJumps];
        // } else {
        //   // blockInfo[oneSourceCycle].scheduleSizeOpt2 = block->vliwEndAddress - block->jumpPlaces[currentjumpId];
        //   otherBlock->sizeOpt2 = block->jumpPlaces[currentjumpId + 1] - block->jumpPlaces[currentjumpId];
        // }
        //
        // currentjumpId++;
        nbBlockAdded++;
      }
    }

    block->sizeOpt2 = block->sizeOpt2 / (block->nbMergedBlocks + 1);

    // if (block->nbJumps == 0) {
    //   block->sizeOpt2 = block->sizeOpt2 / block->nbMergedBlocks;
    // } else {
    //   block->sizeOpt2 = block->jumpPlaces[0] - block->vliwStartAddress;
    // }
  }

  if (nbBlockAdded != block->nbMergedBlocks) {
    printf("Added %d block infor while there are %d merged blocks and %d jumps\n", nbBlockAdded, block->nbMergedBlocks,
           block->nbJumps);
  }
}

IRProcedure* optimizeLevel2(IRBlock& block)
{

  // We check if the block has already been optimized
  if (block.sizeOpt2 == -1) {

    if (block.blockState >= IRBLOCK_ERROR_PROC) {
      if (block.sizeOpt1 == -1)
        block.sizeOpt1 = block.vliwEndAddress - block.vliwStartAddress;

      block.sizeOpt2 = block.vliwEndAddress - block.vliwStartAddress;
      block.sizeOpt2 = block.sizeOpt2;

      return NULL;
    } else {
      int errorCode = buildAdvancedControlFlow(platform, &block, application);

      if (!errorCode) {
        block.blockState = IRBLOCK_PROC;
        buildTraces(platform, application->procedures[application->numberProcedures - 1], 100);
        placeCode = rescheduleProcedure(platform, application,
                                        application->procedures[application->numberProcedures - 1], placeCode);
        updateSpeculationsStatus(platform, application, placeCode);

        for (int oneBlock = 0; oneBlock < application->procedures[application->numberProcedures - 1]->nbBlock;
             oneBlock++) {
          IRBlock* block            = application->procedures[application->numberProcedures - 1]->blocks[oneBlock];
          IRBlock* blockInOtherList = blockInfo[block->sourceStartAddress].block;

          if (block != blockInOtherList)
            blockInfo[block->sourceStartAddress].block = block;

          updateOpt2BlockSize(block);
        }

        return application->procedures[application->numberProcedures - 1];
      } else {
        printf("Met an error while trying to go to opt level 2 for %x (error code %d)\n", block.sourceStartAddress,
               errorCode);
        return NULL;
      }
    }
  } else {
    for (int oneProcedure = 0; oneProcedure < application->numberProcedures; oneProcedure++) {
      IRProcedure* procedure = application->procedures[oneProcedure];
      if (procedure->entryBlock->sourceStartAddress == block.procedureSourceStartAddress)
        return procedure;
    }
    printf("Proc not found\n");
    return NULL;
  }
}

unsigned int addressStart;

void initializeDBTInfo(char* fileName)
{

  int CONFIGURATION = 2;
  int VERBOSE       = 0;
  Log::Init(VERBOSE, 0);

  // We copy the filePath
  execPath = (char*)malloc(strlen(fileName) + 10);
  strcpy(execPath, fileName);

  filenameMetric       = (char*)malloc(strlen(execPath) + 50);
  char* iteratorString = (char*)malloc(strlen(execPath) + 10);

  int execPathLength           = strlen(execPath);
  execPath[execPathLength]     = '.';
  execPath[execPathLength + 1] = 'd';
  execPath[execPathLength + 2] = 'b';
  execPath[execPathLength + 3] = 't';
  execPath[execPathLength + 4] = 0;

  strcpy(iteratorString, execPath);
  iteratorString = strtok(iteratorString, "/");
  char* progname = execPath;

  while (iteratorString != NULL) {
    // fprintf(stderr, "%s\n", iteratorString);
    progname       = iteratorString;
    iteratorString = strtok(NULL, "/");
  }
  strcpy(filenameMetric, "/home/lsavary/Documents/v8metrix/");
  strcat(filenameMetric, progname);
  int len                 = strlen(filenameMetric);

  filenameMetric[len - 3] = 't';
  filenameMetric[len - 2] = 'x';
  filenameMetric[len - 1] = 't';
  fprintf(stderr, "%s\n", filenameMetric);

  char* str_size_tc = getenv("SIZE_TC");
  if (str_size_tc != NULL)
    SIZE_TC = atoi(str_size_tc);
  else {
    SIZE_TC = 0;
    fprintf(stderr, "SIZE_TC not found, set at 0 by default\n");
  }

  nb_max_counter    = 0;
  nb_tc_too_full    = 0;
  nb_tc_too_small   = 0;
  nb_try_proc_in_tc = 0;
  nb_proc_in_tc     = 0;

  /***********************************
   *  Initialization of the DBT platform
   ***********************************
   * In the linux implementation, this is done by reading an elf file and copying binary instructions
   * in the corresponding memory.
   * In a real platform-> this may require no memory initialization as the binaries would already be stored in the
   * system memory.
   ************************************/

  // Definition of objects used for DBT process
  platform = new DBTPlateform(MEMORY_SIZE);

  unsigned char* code;

  unsigned int size;
  unsigned int pcStart;

  readSourceBinaries(fileName, code, addressStart, size, pcStart, platform);

  platform->vliwInitialConfiguration = CONFIGURATION;
  platform->vliwInitialIssueWidth    = getIssueWidth(platform->vliwInitialConfiguration);

  // Preparation of required memories
  for (int oneFreeRegister = 33; oneFreeRegister < 63; oneFreeRegister++)
    platform->freeRegisters[oneFreeRegister - 33] = oneFreeRegister;

  for (int oneFreeRegister = 63 - 33; oneFreeRegister < 63; oneFreeRegister++)
    platform->freeRegisters[oneFreeRegister] = 0;

  for (int onePlaceOfRegister = 0; onePlaceOfRegister < 64; onePlaceOfRegister++)
    platform->placeOfRegisters[256 + onePlaceOfRegister] = onePlaceOfRegister;
  // same for FP registers
  for (int onePlaceOfRegister = 0; onePlaceOfRegister < 64; onePlaceOfRegister++)
    platform->placeOfRegisters[256 + 64 + onePlaceOfRegister] = onePlaceOfRegister;

  platform->vexSimulator = new EmptySimulator(platform->vliwBinaries, platform->specInfo);
  setVLIWConfiguration(platform->vexSimulator, platform->vliwInitialConfiguration);

  // Setting blocks as impossible to destroy
  IRBlock::isUndestroyable = true;

  int numberOfSections            = 1 + (size >> 10);
  application                     = new IRApplication(addressStart, size);
  application->numberInstructions = size;
  Profiler profiler               = Profiler(platform);

  // we copy the binaries in the corresponding memory
  for (int oneInstruction = 0; oneInstruction < size; oneInstruction++)
    platform->mipsBinaries[oneInstruction] = ((unsigned int*)code)[oneInstruction];

  // We declare the variable in charge of keeping a track of where we are writing
  placeCode = 0; // As 4 instruction bundle

  // We add initialization code to the vliw binaries
  placeCode = getInitCode(platform, placeCode, addressStart);
  placeCode = insertCodeForInsertions(platform, placeCode, addressStart);

  initializeInsertionsMemory(size * 4);

  for (int oneSection = 0; oneSection < (size >> 10) + 1; oneSection++) {

    int startAddressSource = addressStart + oneSection * 1024 * 4;
    int endAddressSource   = startAddressSource + 1024 * 4;
    if (endAddressSource > addressStart + size * 4)
      endAddressSource = addressStart + (size << 2);

    int effectiveSize = (endAddressSource - startAddressSource) >> 2;
    for (int j = 0; j < effectiveSize; j++) {
      platform->mipsBinaries[j] = ((unsigned int*)code)[j + oneSection * 1024];
    }
    int oldPlaceCode = placeCode;

    placeCode = translateOneSection(*platform, placeCode, addressStart, startAddressSource, endAddressSource);

    buildBasicControlFlow(platform, oneSection, addressStart, startAddressSource, oldPlaceCode, placeCode, application,
                          &profiler);
  }

  for (int oneUnresolvedJump = 0; oneUnresolvedJump < unresolvedJumpsArray[0]; oneUnresolvedJump++) {
    unsigned int source             = unresolvedJumpsSourceArray[oneUnresolvedJump + 1];
    unsigned int initialDestination = unresolvedJumpsArray[oneUnresolvedJump + 1];
    unsigned int type               = unresolvedJumpsTypeArray[oneUnresolvedJump + 1];

    unsigned char isAbsolute = ((type & 0x7f) != VEX_BR) && ((type & 0x7f) != VEX_BRF && (type & 0x7f) != VEX_BLTU) &&
                               ((type & 0x7f) != VEX_BGE && (type & 0x7f) != VEX_BGEU) && ((type & 0x7f) != VEX_BLT);
    int destinationInVLIWFromNewMethod = solveUnresolvedJump(platform, initialDestination);

    if (destinationInVLIWFromNewMethod == -1) {
      Log::logError << "A jump from " << source << " to " << std::hex << initialDestination
                    << " is still unresolved... (" << insertionsArray[(initialDestination >> 10) << 11]
                    << " insertions)\n";
      exit(-1);
    } else {
      int immediateValue =
          (isAbsolute) ? (destinationInVLIWFromNewMethod) : ((destinationInVLIWFromNewMethod - source));
      int mask = (isAbsolute) ? 0x7ffff : 0x1fff;

      writeInt(platform->vliwBinaries, 16 * (source), type + ((immediateValue & mask) << 7));

      if (immediateValue > 0x7ffff) {

        Log::logError << "Error in immediate size... Should be corrected in real life\n";
        immediateValue &= 0x7ffff;
      }
      unsigned int instructionBeforePreviousDestination =
          readInt(platform->vliwBinaries, 16 * (destinationInVLIWFromNewMethod - 1) + 12);
      if (instructionBeforePreviousDestination != 0)
        writeInt(platform->vliwBinaries, 16 * (source + 1) + 12, instructionBeforePreviousDestination);
    }
  }

  // We allocate blockInfo
  globalBinarySize = 10 * size;
  blockInfo        = (BlockInformation*)malloc(10 * size * sizeof(BlockInformation));
  for (int oneBlockInfo = 0; oneBlockInfo < 10 * size; oneBlockInfo++) {
    blockInfo[oneBlockInfo].block            = NULL;
    blockInfo[oneBlockInfo].scheduleSizeOpt0 = -1;
    blockInfo[oneBlockInfo].scheduleSizeOpt1 = -1;
    blockInfo[oneBlockInfo].scheduleSizeOpt2 = -1;
    blockInfo[oneBlockInfo].nbChargement     = 0; // number of times the block is load in IT
    blockInfo[oneBlockInfo].nbOpti1          = 0; // number of times the block is optimize to level 1
    blockInfo[oneBlockInfo].nbOpti2          = 0; // number of times the block is optimize to level 2
    blockInfo[oneBlockInfo].nbExecution      = 0; // number of times the block is executed
  }

  // We check whether the application has already been optimized
  std::ifstream previousExecDump(execPath);
  if (previousExecDump.good()) {
    greatestAddr = 0;
    for (auto& block : *application) {
      if (block.sourceEndAddress > greatestAddr)
        greatestAddr = block.sourceEndAddress;
    }

    delete (application);
    application = new IRApplication(addressStart, size);

    application->loadApplication(execPath, greatestAddr * 4);

    // We check if application has been optimized...
    bool isOptimized = true;
    for (auto& block : *application) {
      blockInfo[block.sourceStartAddress].block            = &block;
      blockInfo[block.sourceStartAddress].scheduleSizeOpt0 = block.sizeOpt0;
      blockInfo[block.sourceStartAddress].scheduleSizeOpt1 = block.sizeOpt1;
      blockInfo[block.sourceStartAddress].scheduleSizeOpt2 = block.sizeOpt2;

      // printf("For block %x, %d %d %d  (unrolling = %d, trace = %d)\n", block.sourceStartAddress, block.sizeOpt0,
      //        block.sizeOpt1, block.sizeOpt2, block.unrollingFactor, block.nbMergedBlocks);

      if (block.sizeOpt2 == -1) {
        isOptimized = false;
        break;
      }
    }

    if (!isOptimized) {
      printf("Starting level 1 optimizations...\n");

      // We count the number of blocks
      int nbBlocks = 0;
      for (auto& block : *application)
        nbBlocks++;

      unsigned int step             = nbBlocks / 100;
      unsigned int nbBlockOptimized = 0;
      for (auto& block : *application) {
        if (block.blockState == IRBLOCK_STATE_FIRSTPASS) {
          optimizeBasicBlock(&block, platform, application, placeCode);
          block.sizeOpt1 = block.vliwEndAddress - block.vliwStartAddress;
        }

        // Drawing progress bar
        nbBlockOptimized++;
        if (nbBlockOptimized % step == 0) {
          unsigned int progress = nbBlockOptimized / step;
          int oneStep;
          printf("\r");
          for (oneStep = 0; oneStep < 100; oneStep++) {
            if (oneStep < progress)
              printf("=");
            else if (oneStep == progress)
              printf(">");
            else
              printf(" ");
          }

          printf("  %d / %d", nbBlockOptimized, nbBlocks);
        }
      }

      printf("\nStarting level 2 optimizations...\n");

      step             = nbBlocks / 100;
      nbBlockOptimized = 0;
      for (auto& block : *application) {
        if (block.blockState <= IRBLOCK_STATE_SCHEDULED) {
          optimizeLevel2(block);
        }

        // Drawing progress bar
        nbBlockOptimized++;
        if (nbBlockOptimized % step == 0) {
          unsigned int progress = nbBlockOptimized / step;
          int oneStep;
          printf("\r");
          for (oneStep = 0; oneStep < 100; oneStep++) {
            if (oneStep < progress)
              printf("=");
            else if (oneStep == progress)
              printf(">");
            else
              printf(" ");
          }

          printf("  %d / %d", nbBlockOptimized, nbBlocks);
        }
      }
      printf("\nDone!\n");

      for (auto& block : *application) {
        if (block.sizeOpt0 == -1) {
          printf("For block %d, opt 0 is not computed\n", block.sourceStartAddress);
        }

        if (block.sizeOpt1 == -1) {
          printf("For block %d, opt 1 is not computed\n", block.sourceStartAddress);
        }

        if (block.sizeOpt2 == -1) {
          printf("For block %d, opt 2 is not computed (state is %d)\n", block.sourceStartAddress, block.blockState);
          block.sizeOpt2 = block.sizeOpt1;
        }
      }

      application->dumpApplication(execPath, greatestAddr * 4);
    }

    printf("Loaded an existing dump!\n");

  } else {
    isExploreOpts = true;
  }

  /********************** We store the size of each block ***********************/

  greatestAddr       = 0;
  int numberOfBlocks = 0;
  for (auto& block : *application) {
    blockInfo[block.sourceStartAddress].block            = &block;
    blockInfo[block.sourceStartAddress].scheduleSizeOpt0 = block.sizeOpt0;
    blockInfo[block.sourceStartAddress].scheduleSizeOpt1 = block.sizeOpt1;
    blockInfo[block.sourceStartAddress].scheduleSizeOpt2 = block.sizeOpt2;
    if (block.sourceEndAddress > greatestAddr)
      greatestAddr = block.sourceEndAddress;

    numberOfBlocks++;
  }
  /************************** We create a copy of the platform which is used to re-optimize ***********************/
  //
  // nonOptPlatform = (DBTPlateform*)malloc(sizeof(DBTPlateform));
  // memcpy(nonOptPlatform, platform, sizeof(DBTPlateform));
  //
  // nonOptPlatform->vliwBinaries = (unsigned int*)malloc(4 * MEMORY_SIZE * sizeof(unsigned int));
  // memcpy(nonOptPlatform->vliwBinaries, platform->vliwBinaries, 4 * MEMORY_SIZE * sizeof(unsigned int));
}

/******************************************************************************************************
 *  Function useIndirectionTable(int address)
 ******************************************************************************************************
 * This function is the first function called by Qemu to verify whether the current branch should be
 * considered like a traced jump or not.
 * If it is, we have to go through all the indirection table, otherwise we stay in the same mode and opt
 * level than before.
 *
 * The function just check the optimization level of the said block : if it is level 2 then the jump
 * are no longer checked
 *
 ******************************************************************************************************/

char useIndirectionTable(int address)
{
  // Address is the RISCV address of the jump
  // We find the nearest block above
  int currentAddress = address;
  while (blockInfo[currentAddress >> 2].block == NULL) {
    currentAddress -= 4;
  }

  if (blockInfo[currentAddress >> 2].block->blockState > IRBLOCK_STATE_SCHEDULED) {
    if (blockInfo[currentAddress >> 2].block->nbSucc > 0)
      return 0;
  }

  return 1;
}

/******************************************************************************************************
 *  Function getBlockSize (int address, int optLevel, int timeFromSwitch, int *nextBlock)
 ******************************************************************************************************
 * This function is used to get size of a given block (and thus its execution time). This function is
 * the last one called. Consequently, we know the exact opt level to use for it.
 *
 * The function receive an address and an optimization level and find the corresponding block size.
 * The function also modifies nextBlock in order to provide the address of the next block to execute.
 *
 ******************************************************************************************************/

int getBlockSize(int address, int optLevel, int timeFromSwitch, int* nextBlock)
{

  // printf("Asking for %d\n", address>>2);
  if ((address >> 2) >= globalBinarySize) {
    fprintf(stderr, "too large !!\n");
    return 0;
  }

  if (blockInfo[address >> 2].block == NULL) {
    printf("Add to correct address of block (was %x)\n", address);
    int currentAddress = address;
    while (blockInfo[currentAddress >> 2].block == NULL) {
      currentAddress -= 4;
    }
    if (currentAddress - address < 9)
      address = currentAddress;
    else {
      fprintf(stderr, "Failed at finding block at %x   nearest is in %x\n", address >> 2, currentAddress >> 2);
      return 0;
    }
  }

  unsigned int end = blockInfo[address >> 2].block->sourceEndAddress;
  while (blockInfo[end].block == NULL) {
    end++;
  }
  *nextBlock = end * 4;
  blockInfo[address >> 2].nbExecution++;

  if (optLevel == 1) { // Opt level 1
    if (blockInfo[address >> 2].scheduleSizeOpt1 == -1) {
      // We have to schedule the block
      optimizeBasicBlock(blockInfo[address >> 2].block, platform, application, placeCode);
      blockInfo[address >> 2].block->sizeOpt1 =
          blockInfo[address >> 2].block->vliwEndAddress - blockInfo[address >> 2].block->vliwStartAddress;
      blockInfo[address >> 2].scheduleSizeOpt1 =
          blockInfo[address >> 2].block->vliwEndAddress - blockInfo[address >> 2].block->vliwStartAddress;
    }

    return blockInfo[address >> 2].scheduleSizeOpt1;
  } else if (optLevel >= 2) { // Opt level 2

    if (blockInfo[address >> 2].scheduleSizeOpt2 == -1) {
      if (blockInfo[address >> 2].block->blockState >= IRBLOCK_STATE_SCHEDULED)
        updateOpt2BlockSize(blockInfo[address >> 2].block);
      else {
        optimizeBasicBlock(blockInfo[address >> 2].block, platform, application, placeCode);
        blockInfo[address >> 2].scheduleSizeOpt1 =
            blockInfo[address >> 2].block->vliwEndAddress - blockInfo[address >> 2].block->vliwStartAddress;
        blockInfo[address >> 2].block->sizeOpt1 = blockInfo[address >> 2].scheduleSizeOpt1;

        updateOpt2BlockSize(blockInfo[address >> 2].block);
      }
    }

    //		if (blockInfo[address>>2].block->blockState == IRBLOCK_UNROLLED)
    //			fprintf(stderr, "Executing an unrolled loop -- Cost is %d instead of %d\n",
    // blockInfo[address>>2].scheduleSizeOpt2,
    // blockInfo[address>>2].block->vliwEndAddress-blockInfo[address>>2].block->vliwStartAddress);

    // We just return the size of the block at optimization level 2
    return blockInfo[address >> 2].scheduleSizeOpt2;
  }

  if (blockInfo[address >> 2].scheduleSizeOpt0 == 0)
    fprintf(stderr, "size 0 is equal to zero for %d\n", address >> 2);
  // If we are neither at opt level 1 or 2, we return the size at opt level 0

  return blockInfo[address >> 2].scheduleSizeOpt0;
}

/**********************************************************************************
 *  Function getOptLevel(int address, uint64_t nb_cycle)
 ****************************
 * This function is the first called after a jump is simulated in QEMU. The objective
 * is to simulate the indirection table and to decide whether or not the execution goes
 * through the translation cache.
 * This function should go through:
 * 	+ Simulation of the indirection table
 * 	+ Simulation of the translation cache & of the replacement policy
 * 	+ Simulation of the return stack
 *
 **********************************************************************************/

char getOptLevel(int address, uint64_t nb_cycle)
{
  if (isExploreOpts)
    return 0;

  bool inTranslationCache = false;
  char optLevel           = 0;
  nb_cycle_for_eval = nb_cycle;
  /************************************************************************************
   *	Step 1: Simulation of the indirection table: is the branch profiled ? 			*/

  int setNumber = (address >> 2) & 0x7;
  bool found    = 0;
  for (int oneWay = 0; oneWay < IT_NB_WAY; oneWay++) {
    if (indirectionTable[oneWay][setNumber].address == address) {
      // The destination of the branch is in the indirection table.

      if (indirectionTable[oneWay][setNumber].counter < MAX_IT_COUNTER) {
        // We increment the use counter
        indirectionTable[oneWay][setNumber].counter++;
        if (indirectionTable[oneWay][setNumber].counter == MAX_IT_COUNTER)
          nb_max_counter ++;
      }
      if (indirectionTable[oneWay][setNumber].isInTC) {
        if (indirectionTable[oneWay][setNumber].timeAvailable < nb_cycle)
          optLevel = indirectionTable[oneWay][setNumber].optLevel;
        else
          optLevel = indirectionTable[oneWay][setNumber].optLevel - 1;
      }

      indirectionTable[oneWay][setNumber].lastCycleTouch = nb_cycle;
      found                                              = true;
    }
  }

  if (!found) {
    inTranslationCache = false;
    bool putInIT       = 1;
    for (int oneWay = 0; oneWay < IT_NB_WAY; oneWay++) {
      if (putInIT == 1 && indirectionTable[oneWay][setNumber].counter <= 0) {
        indirectionTable[oneWay][setNumber].address        = address;
        indirectionTable[oneWay][setNumber].counter        = 1;
        indirectionTable[oneWay][setNumber].optLevel       = 0;
        indirectionTable[oneWay][setNumber].timeAvailable  = nb_cycle;
        indirectionTable[oneWay][setNumber].lastCycleTouch = nb_cycle;

        blockInfo[address >> 2].nbChargement++;

        putInIT = 0;
      } else if (indirectionTable[oneWay][setNumber].counter > 0) {

        indirectionTable[oneWay][setNumber].counter--;
      }
    }
  }

  /************************************************************************************
   *	Step 3: Simulation of the call stack: we have to decide whether the return goes *
   *			inside the translation cache or inside the first pass translation		*/

  // TODO


  if (optLevel < 0)
    optLevel = 0;
  return optLevel;
}

void triggerOptimization(uint64_t nb_cycle) {

  /***********************************************************************************
   * Triggering the optimization if the DBT proc is available						   */

  if (nextAvailabilityDBTProc <= nb_cycle) {
    bool search_entry_to_opt = true;
    float max_eval = float(MAX_IT_COUNTER)*10.0f;

    while (search_entry_to_opt) {
      // find the more valuable IT entry to optimize ---------------------------
      float best_eval = 0; // in the interval [0;MAX_IT_COUNTER]
      std::vector<int> sets, ways;

      for (int oneWay = 0; oneWay < IT_NB_WAY; oneWay++) {
        for (int oneSet = 0; oneSet < IT_NB_SET; oneSet++) {
          if (indirectionTable[oneWay][oneSet].counter >= THRESHOLD_OPTI1 && indirectionTable[oneWay][oneSet].optLevel < 2) {
            struct indirectionTableEntry entry = indirectionTable[oneWay][oneSet];
            // int size = 1;
            // evaluation :
            if (entry.optLevel == 1) {
              IRProcedure * proc = optimizeLevel2(*blockInfo[entry.address >> 2].block);
              if (proc != NULL && ( proc->nbInstr > SIZE_TC || proc->nbInstr > 13000 )) {
                continue;
                // the level 2 optimization is too big for the tc, no need to evaluate
              }
              /*
              if (proc != NULL)
                size = proc->nbInstr;
              */
            } /*else {
              size = blockInfo[entry.address >> 2].block->sourceEndAddress -
                     blockInfo[entry.address >> 2].block->sourceStartAddress;
            }*/
            float eval = float(entry.counter) * float(entry.lastCycleTouch) / float(nb_cycle);
            //fprintf(stderr, "eval %f\n",eval);
            if (eval - best_eval < FLOAT_PRECISION && eval - best_eval > -1.0 * FLOAT_PRECISION) {
              sets.push_back(oneSet);
              ways.push_back(oneWay);
            } else if (eval - best_eval >= FLOAT_PRECISION && max_eval - eval > FLOAT_PRECISION) {
              sets.clear();
              ways.clear();
              best_eval = eval;
              sets.push_back(oneSet);
              ways.push_back(oneWay);
            }
          }
        }
      }
      // -----------------------------------------------------------------------

      if (best_eval <= FLOAT_PRECISION) {
        break;
        // leave the while
      }

      for (int i = 0; i < sets.size() && search_entry_to_opt; i++) {

        indirectionTableEntry * entry_to_opt = &(indirectionTable[ways[i]][sets[i]]);
        unsigned int oneAddress = entry_to_opt->address;
        if (entry_to_opt->counter >= THRESHOLD_OPTI2 && entry_to_opt->optLevel <= 1) {
         // We trigger opt level 2

          if (blockInfo[oneAddress >> 2].block != NULL &&
              blockInfo[oneAddress >> 2].block->blockState < IRBLOCK_ERROR_PROC) {

            IRProcedure* procedure = optimizeLevel2(*blockInfo[oneAddress >> 2].block);

            // We compute the size
            if (procedure != NULL) {

              // We measure the sum of all blocks in the procedure
              int size = procedure->nbInstr;
              bool fitsInTranslationCache = allocateInTranslationCache(size, procedure, NULL);
              nb_try_proc_in_tc ++;
              // We see if it fits the TC
              if (fitsInTranslationCache) {
                entry_to_opt->optLevel       = 2;
                entry_to_opt->isInTC         = true;
                entry_to_opt->timeAvailable  = nb_cycle + COST_OPT_2 * size;
                entry_to_opt->costOfBinaries = COST_OPT_2 * size;
                entry_to_opt->sizeInTC       = size;
                entry_to_opt->lastCycleTouch = nb_cycle;

                nb_proc_in_tc ++;
                nb_traduction ++;
                // fprintf(stderr, "%d\n", size);
                search_entry_to_opt = false;
                nextAvailabilityDBTProc = nb_cycle + COST_OPT_2 * size;
                for (int oneBlock = 0; oneBlock < procedure->nbBlock; oneBlock++) {
                  blockInfo[procedure->blocks[oneBlock]->sourceStartAddress].nbOpti2++;
                }

                //redirect the others blocks of the procedure on the optimised procedure
                for (int w = 0; w < IT_NB_WAY; w ++)
                  for (int s = 0; s < IT_NB_SET; s ++) {
                    IRBlock * block = blockInfo[indirectionTable[w][s].address >> 2].block;
                    if ( block != NULL && block->procedureSourceStartAddress == procedure->entryBlock->sourceStartAddress)
                      indirectionTable[w][s].optLevel = 2;
                  }


              }
            }
          }
        }
        if (entry_to_opt->counter >= THRESHOLD_OPTI1 && entry_to_opt->optLevel <= 0 && search_entry_to_opt ) {
          // We should trigger opt level 1


          if (blockInfo[oneAddress >> 2].block != NULL) {

            int size = blockInfo[oneAddress >> 2].block->sourceEndAddress -
                       blockInfo[oneAddress >> 2].block->sourceStartAddress;
            bool fitsInTranslationCache = allocateInTranslationCache(size, NULL, blockInfo[oneAddress >> 2].block);

            if (fitsInTranslationCache) {
              entry_to_opt->optLevel       = 1;
              entry_to_opt->isInTC         = true;
              entry_to_opt->timeAvailable  = nb_cycle + COST_OPT_1 * size;
              entry_to_opt->costOfBinaries = COST_OPT_1 * size;
              entry_to_opt->sizeInTC       = size;
              entry_to_opt->lastCycleTouch = nb_cycle;
              nextAvailabilityDBTProc = nb_cycle + COST_OPT_1 * size;

              nb_traduction++;
              search_entry_to_opt = false;
              blockInfo[oneAddress >> 2].nbOpti1++;
            }
          }
        }
      }

      //we need another thing to optimize
      max_eval = best_eval;

    }//while
  }//if



  if (lastAvailability < nextAvailabilityDBTProc && timescaledLog != NULL) {
    lastAvailability = nextAvailabilityDBTProc;
    int nb_element = 0;
    int size_last_tc_content = 0;
    int opt_level = 0;
    uint64_t block_address = 0;
    if (translationCacheContent != NULL) {
      nb_element = translationCacheContent->size();
      size_last_tc_content = translationCacheContent->at(nb_element-1).size;
      opt_level = (translationCacheContent->at(nb_element-1).isBlock ? 1 : 2);
      if (opt_level == 1) {
        block_address = translationCacheContent->at(nb_element -1).block->sourceStartAddress;
      } else {
        block_address = translationCacheContent->at(nb_element -1).procedure->entryBlock->sourceStartAddress;
      }
    }
    int tc_filling = 0, tc_o2_filling = 0;
    for (int tc_entry = 0; tc_entry < translationCacheContent->size(); tc_entry ++) {
      tc_filling += translationCacheContent->at(tc_entry).size;
      if (!translationCacheContent->at(tc_entry).isBlock)
        tc_o2_filling += translationCacheContent->at(tc_entry).size;
    }

    fprintf(timescaledLog, "%lu,%lu,%d,%d,%lu,%d,%d\n", nb_cycle, nextAvailabilityDBTProc, tc_filling, tc_o2_filling, block_address, size_last_tc_content, opt_level);
  }



}//triggerOptimization


bool allocateInTranslationCache(int size, IRProcedure* procedure, IRBlock* block)
{
  count_alloc++;

  if (size > SIZE_TC && SIZE_TC != 0) {
    nb_tc_too_small ++;
    // it is not even worth the try
    return false;
  }

  // reject too large procedures, because it take too long to optimize it
  // (ex : heat-3d has a 20 000 instr procedure -> it takes 40M cycles to optimize)
  if (size > 13000) {
    fprintf(stderr, "too large procedure : not put in tc \n");
    return false;
  }
  // fprintf(stderr, "\rallocateInTrans 1 " );
  // If first use, we initialize the data structure
  if (translationCacheContent == NULL)
    translationCacheContent = new std::vector<struct entryInTranslationCache>();

  // We create the new element
  struct entryInTranslationCache newEntry;
  newEntry.procedure = procedure;
  newEntry.block     = block;
  newEntry.size      = size;
  newEntry.cost      = size;
  if (procedure != NULL) {
    newEntry.isBlock = false;
    newEntry.cost *= COST_OPT_2;
  } else {
    newEntry.isBlock = true;
    newEntry.cost *= COST_OPT_1;
  }

  int currentSize = contentTranslationCache();
  if (SIZE_TC == 0 || currentSize + size < SIZE_TC) {
    // We can store the translated element in the translation cache without having to evict anything
    translationCacheContent->push_back(newEntry);
    // fprintf(stderr, "allocateInTranslationCache : il y a la place\n" );
    return true;
  } else {
    if (size > SIZE_TC) {
      nb_tc_too_small ++;
      // fprintf(stderr, "allocateInTranslationCache : entry do not fit in the tc %d\n", size);
      return false;
    }
    // We have to evict something


    int nbElement                = translationCacheContent->size();
    std::vector<int> evalValues;
    std::vector<int> waysInIT ;
    std::vector<int> setsInIT ;
    int idmax = 0, valmax = 0;

    int default_eviction_size = 0;
    // set up the vectors
    // and start by remove elements which are not in the IT anymore
    for (int entry = 0; entry < nbElement; entry++) {
      int way = -1, set = -1;
      int entryEval = evalFunction(translationCacheContent->at(entry), &way, &set);

      if (entryEval == 0) {

        // we evict all elements that the IT do not target anymore
        default_eviction_size += translationCacheContent->at(entry).size;
        translationCacheContent->erase(translationCacheContent->begin() + entry);
        entry--;
        nbElement --;


      } else {
        evalValues.push_back(entryEval);
        waysInIT.push_back(way);
        setsInIT.push_back(set);
        if (valmax < entryEval) {
          valmax = entryEval;
          idmax  = entry;
        }
      }
    }
    // fprintf(stderr, "currentSize : %d, default_eviction_size : %d\n",currentSize, default_eviction_size );
    currentSize -= default_eviction_size;
    /*
        // display
        fprintf(stderr,"stocked translation : %d, size of the new entry : %d\n", currentSize, size);
        for ( int i = 0; i < translationCacheContent->size(); i ++) {
          struct entryInTranslationCache theEntry = translationCacheContent->at(i);
          if (theEntry.procedure == NULL) fprintf(stderr,"\033[0;33m");
          else fprintf(stderr,"\033[0;32m");
          fprintf(stderr,"%d(%d) ", theEntry.size, waysInIT[i]);
        }
        fprintf(stderr,"\033[0m\n");
    */
    /*
        fprintf(stderr,"\033[31m");
        fprintf(stderr, "IndirectionTable : \n" );
        for (int set = 0; set < IT_NB_SET; set ++) {
          for(int way = 0; way < IT_NB_WAY; way ++)
            fprintf(stderr, "%d\t", (indirectionTable[way][set].address));
          fprintf(stderr, "\n" );
        }
        fprintf(stderr,"\033[0m");
    */

    int tmp; // we do not need the IT coords of the new entry
    int newEntryEval = evalFunction(newEntry, &tmp, &tmp);

    // select elements to make space
    int sumSize      = 0; // sum of removed components sizes
    int nbSelected   = 0; // number of elements selected to be removed from tc
    while (currentSize + size > SIZE_TC + sumSize) {
      int idmin = idmax, valmin = valmax;

      // find the less valuable entry in translation cache
      for (int i = 0; i < nbElement - nbSelected; i++)
        if (waysInIT[i] < valmin) {

          idmin  = i;
          valmin = waysInIT[i];
        }

      if (valmin > newEntryEval) { // => can't make enough space in tc
        if (newEntry.block == NULL) {
          fprintf(stderr, "procedure refusee : eval %d\n", newEntryEval);
        }
        nb_tc_too_full ++;
        return false;
      }

      sumSize += translationCacheContent->at(idmin).size;
      nbSelected++;
      // palce the element to be removed at the end of the tc
      struct entryInTranslationCache tmpEntry             = translationCacheContent->at(nbElement - nbSelected);
      translationCacheContent->at(nbElement - nbSelected) = translationCacheContent->at(idmin);
      translationCacheContent->at(idmin)                  = tmpEntry;
    }

    // adjust selection : restore the more valuable elements not needed to be removed
    for (int i = nbElement - nbSelected; i < nbElement; i++) {
      if (size + currentSize <= SIZE_TC + sumSize - translationCacheContent->at(i).size) {
        sumSize -= translationCacheContent->at(i).size;
        // swap elements, to keep the removables at the end of the vector
        struct entryInTranslationCache tmpEntry             = translationCacheContent->at(nbElement - nbSelected);
        translationCacheContent->at(nbElement - nbSelected) = translationCacheContent->at(i);
        translationCacheContent->at(i)                      = tmpEntry;
        nbSelected--;
      }
    }

    // fprintf(stderr, "suppressed components (%d to remove): ", nbSelected);
    for (int anEntry = nbElement - nbSelected; anEntry < nbElement; anEntry++) {
      struct entryInTranslationCache theEntry = translationCacheContent->at(anEntry);
      currentSize -= theEntry.size;

      // fprintf(stderr, "%d(%d) ",theEntry.size, waysInIT[anEntry]);
      // processing of the eviction
      if (waysInIT[anEntry] > -1 && setsInIT[anEntry] > -1) {
        indirectionTable[waysInIT[anEntry]][setsInIT[anEntry]].counter  = 0;
        indirectionTable[waysInIT[anEntry]][setsInIT[anEntry]].optLevel = 0;
        indirectionTable[waysInIT[anEntry]][setsInIT[anEntry]].isInTC   = 0;
      }
    }
    // fprintf(stderr, "\n\n" );

    translationCacheContent->erase(translationCacheContent->end() - nbSelected, translationCacheContent->end());

    if (procedure != NULL) nb_proc_in_tc ++;

    // store the new element
    translationCacheContent->push_back(newEntry);
    currentSize += size;
    return true;
  }
}

void verifyBranchDestination(int addressOfJump, int dest)
{

  if (blockInfo[dest >> 2].block == NULL) {
    // The destination of the jumps is not a block entry point. We have to modify everything.
    fprintf(stderr, "Correcting a wrong block boundary (address is %x)!\n", dest);

    unsigned int correspondingVliwAddress = solveUnresolvedJump(platform, (dest - addressStart) / 4);

    // We find the corresponding block
    int currentStart = dest >> 2;
    while (blockInfo[currentStart].block == NULL)
      currentStart--;

    IRBlock* containingBlock = blockInfo[currentStart].block;
    assert((dest >> 2) < containingBlock->sourceEndAddress);

    int initialState = containingBlock->blockState;

    // Specifying information concerning the newly created block
    IRBlock* newBlock            = new IRBlock(correspondingVliwAddress,
                                    solveUnresolvedJump(platform, containingBlock->sourceEndAddress - addressStart / 4),
                                    containingBlock->section);
    newBlock->sourceStartAddress = dest >> 2;
    newBlock->sourceEndAddress   = containingBlock->sourceEndAddress;
    newBlock->sourceDestination  = containingBlock->sourceDestination;
    newBlock->blockState         = IRBLOCK_STATE_FIRSTPASS;

    application->addBlock(newBlock);

    // We modify the containing block (only source address and source destination)
    containingBlock->sourceDestination = -1;
    containingBlock->sourceEndAddress  = dest >> 2;

    // We update blockInfo data
    blockInfo[newBlock->sourceStartAddress].block            = newBlock;
    blockInfo[newBlock->sourceStartAddress].scheduleSizeOpt0 = newBlock->vliwEndAddress - newBlock->vliwStartAddress;
    blockInfo[containingBlock->sourceStartAddress].scheduleSizeOpt0 =
        solveUnresolvedJump(platform, containingBlock->sourceEndAddress - addressStart / 4) -
        solveUnresolvedJump(platform, containingBlock->sourceStartAddress - addressStart / 4);
    containingBlock->sizeOpt0 = blockInfo[containingBlock->sourceStartAddress].scheduleSizeOpt0;
    newBlock->sizeOpt0        = blockInfo[newBlock->sourceStartAddress].scheduleSizeOpt0;

    if (blockInfo[newBlock->sourceStartAddress].scheduleSizeOpt0 < 0 ||
        blockInfo[containingBlock->sourceStartAddress].scheduleSizeOpt0 < 0) {
      printf("Error on block splitting, size is lower than zero\n");
      assert(blockInfo[newBlock->sourceStartAddress].scheduleSizeOpt0 >= 0 &&
             blockInfo[containingBlock->sourceStartAddress].scheduleSizeOpt0 >= 0);
    }
    fprintf(stderr, "While correcting a block %d -- %d = %d -- %d\n", containingBlock->vliwStartAddress,
            containingBlock->vliwEndAddress, newBlock->vliwStartAddress, newBlock->vliwEndAddress);

    if (initialState > IRBLOCK_STATE_FIRSTPASS) {

      //NOTE SIMON: we should never enter this section with the new execution scheme.
      assert(false);


      unsigned int oldVliwStartAddress = containingBlock->vliwStartAddress;
      unsigned int oldVliwEndAddress   = containingBlock->vliwEndAddress;

      containingBlock->vliwStartAddress =
          solveUnresolvedJump(platform, containingBlock->sourceStartAddress - addressStart / 4);
      containingBlock->vliwEndAddress =
          solveUnresolvedJump(platform, containingBlock->sourceEndAddress - addressStart / 4);
      containingBlock->blockState = IRBLOCK_STATE_FIRSTPASS;

      // optimizeBasicBlock(containingBlock, nonOptPlatform, application, placeCode);
      // optimizeBasicBlock(newBlock, nonOptPlatform, application, placeCode);

      // We add information on block size
      blockInfo[containingBlock->sourceStartAddress].scheduleSizeOpt1 =
          containingBlock->vliwEndAddress - containingBlock->vliwStartAddress;
      ;
      blockInfo[newBlock->sourceStartAddress].scheduleSizeOpt1 = newBlock->vliwEndAddress - newBlock->vliwStartAddress;
      containingBlock->sizeOpt1 = blockInfo[containingBlock->sourceStartAddress].scheduleSizeOpt1;
      newBlock->sizeOpt1        = blockInfo[newBlock->sourceStartAddress].scheduleSizeOpt1;

      // We restore previous values of vliw adresses
      containingBlock->vliwEndAddress   = oldVliwEndAddress;
      containingBlock->vliwStartAddress = oldVliwStartAddress;

      if (containingBlock->blockState >= IRBLOCK_ERROR_PROC) {
        // The containing block was optimized at proc level, we have to mark the new block as impossible to optimize.
        blockInfo[newBlock->sourceStartAddress].scheduleSizeOpt2 =
            blockInfo[newBlock->sourceStartAddress].scheduleSizeOpt1;
        newBlock->sizeOpt2   = newBlock->sizeOpt1;
        newBlock->blockState = IRBLOCK_ERROR_PROC;
      }
    }
  }
}

void finalizeDBTInformation()
{

  if (isExploreOpts) {
    application->dumpApplication(execPath, greatestAddr * 4);
  }

  // --------------------- Metric ------------------------------------------------
  FILE* metrix;
  metrix = fopen(filenameMetric, "w");
  if (metrix == NULL) {
    printf("ERRROR FILE %s\n", filenameMetric);
  } else {
    for (unsigned int i = 0; i < application->numberProcedures; i++) {
      IRProcedure* proc = application->procedures[i];
      unsigned int size = 0;
      for (int oneBlock = 0; oneBlock < proc->nbBlock; oneBlock++) {
        size += proc->blocks[oneBlock]->nbInstr;
      }
      fprintf(metrix, "%u\t", size);
    }
    fprintf(metrix, "\n");
    fprintf(metrix, "nb_max_counter : \t%u\tnb_tc_too_full : \t%u\tnb_tc_too_small : \t%u\tnb_try_proc_in_tc : \t%u\tnb_proc_in_tc : \t%u\n",
      nb_max_counter, nb_tc_too_full, nb_tc_too_small, nb_try_proc_in_tc, nb_proc_in_tc);
    // fprintf(metrix, "adBlock\tnbIT\tnbO1\tnbO2\tnbExec\tblockSize\n");
    IRApplicationBlocksIterator blockIterator = application->begin(), end = application->end();

    while (blockIterator != end) {
      struct BlockInformation block = blockInfo[(*blockIterator).sourceStartAddress];
      if (block.nbExecution > 0 || block.nbChargement > 0 || block.nbOpti2 > 0) {
        int nbInstr = block.block->sourceEndAddress - block.block->sourceStartAddress;
        fprintf(metrix, "%d\t%d\t%d\t%d\t%d\t%d\n", block.block->sourceStartAddress, block.nbChargement, block.nbOpti1,
                block.nbOpti2, block.nbExecution, nbInstr);
      }
      ++blockIterator;
    }
    fprintf(metrix, "end\n");
    fclose(metrix);

  }
  // -----------------------------------------------------------------------------
  delete(platform);
  delete(application);
  delete(blockInfo);
  delete(filenameMetric);
  delete(execPath);
}


/**
*   grade an entry of the TC by its IT counter, optLevel and lastCycleTouch
*
*   int* way and set are used to return the IT coords of the entry
*
*/
int evalFunction(struct entryInTranslationCache entry, int* way, int* set)
{
  std::vector<unsigned int> addresses;
  if (entry.block != NULL) {
    addresses.push_back(entry.block->sourceStartAddress);
  } else if (entry.procedure != NULL) {
    for (int i = 0; i < entry.procedure->nbBlock; i ++)
    addresses.push_back( entry.procedure->blocks[i]->sourceStartAddress);
  }
  int eval = 0;
  for (int i = 0; i < addresses.size(); i ++) {
    unsigned int address = addresses[i];
    unsigned int lastTouch = nb_cycle_for_eval;
    unsigned int counter   = 1;
    char optLevel          = -1;
    int setvalue           = (address)&0x7;
    address                = address << 2;
    // fprintf(stderr, "%d ", address);
    for (int oneWay = 0; oneWay < IT_NB_WAY; oneWay++) {
      if (indirectionTable[oneWay][setvalue].address == address) {
        //  fprintf(stderr, "Element found in IT : [ %d %d ] : %d\n",oneWay,setvalue,address );
        lastTouch-= indirectionTable[oneWay][setvalue].lastCycleTouch;
        counter   = indirectionTable[oneWay][setvalue].counter;
        optLevel  = indirectionTable[oneWay][setvalue].optLevel;
        *way      = oneWay;
        *set      = setvalue;
        // fprintf(stderr, "%d\t%d\t%u\t%d\t%u\n", *way, *set, lastTouch, optLevel, counter );
        break;
      }
    }
    // if (optLevel == -1) return 0;
    // eval += ((optLevel + 1) * (optLevel + 1) * (counter + 1)) / (lastTouch==0?1:lastTouch);
    if (optLevel != -1) {
      eval += ((optLevel + 1) * (optLevel + 1) * (counter + 1)) * 10000000 / (lastTouch + 10000000);
    }
  }
  return eval;
}
