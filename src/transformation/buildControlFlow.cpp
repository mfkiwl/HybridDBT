/*
 * BuildControlFlow.cpp
 *
 *  Created on: 29 nov. 2016
 *      Author: Simon Rokicki
 */

#include <stdio.h>

#include <dbt/dbtPlateform.h>
#include <dbt/insertions.h>
#include <dbt/profiling.h>

#include <isa/irISA.h>
#include <isa/vexISA.h>

#include <stdlib.h>
#include <string.h>

#include <types.h>
#include <lib/endianness.h>

#include <transformation/irGenerator.h>

#define TEMP_PROCEDURE_STORAGE_SIZE 50
#define TEMP_BLOCK_STORAGE_SIZE 900


void buildBasicControlFlow(DBTPlateform *dbtPlateform, int section, int mipsStartAddress, int sectionStartAddress, int startAddress, int endAddress, IRApplication *application, Profiler *profiler){

	int sizeNewlyTranslated = endAddress-startAddress;
	mipsStartAddress = mipsStartAddress>>2;


	//This first step consists of mapping insertions in order to easily say if isntr n is an insertion or not.

	char* insertionMap = (char*) malloc(sizeNewlyTranslated);
	for (int oneInstruction = 0; oneInstruction < sizeNewlyTranslated; oneInstruction++)
		insertionMap[oneInstruction] = 0;

	int** insertions = (int**) malloc (sizeof(int**));
	int numberInsertions = getInsertionList((sectionStartAddress>>2) - mipsStartAddress, insertions); //TODO
	for (int oneInsertion = 0; oneInsertion < numberInsertions; oneInsertion++){
		//We mark the destination as an insertion
		int index = (*insertions)[oneInsertion];
		insertionMap[index] = 1;
	}
	free(insertions);

	//We declare a temporary storage for storing procedures.
	application->blocksInSections[section] = (IRBlock**) malloc(TEMP_BLOCK_STORAGE_SIZE * sizeof(IRBlock**));
	application->numbersAllocatedBlockInSections[section] = TEMP_BLOCK_STORAGE_SIZE;

	int blockCounter = 0;

	int indexInMipsBinaries = 0;
	int indexInVLIWBinaries = 0;
	int previousBlockStart = 0;
	int previousBlockStartSource = 0;

	//Online resolution of jumps
	unsigned int unresolvedJumpIndex = 0;



	for (int oneInstruction = 0; oneInstruction <= sizeNewlyTranslated; oneInstruction++){
		int offset = (indexInMipsBinaries + ((sectionStartAddress>>2) - mipsStartAddress));
		char blockBoundary = 1;
		if (oneInstruction != sizeNewlyTranslated)
			blockBoundary = (dbtPlateform->blockBoundaries[offset]);
		char isInsertion = (oneInstruction == sizeNewlyTranslated) ? 0 : insertionMap[oneInstruction];

		if (blockBoundary && !isInsertion & previousBlockStart<indexInVLIWBinaries){


			/******************************************************************************************
			 ******************************  Creation and insertion of the block
			 ******************************************************************************************
			 * A new boundary has been reached, we have to create the IRBlock, to give him its values
			 * (eg. vliwStartAddress, vliwEndAddress, sourceStartAddress, sourceEndAddress etc...) and
			 * we insert it in the IRApplication
			 *
			 ******************************************************************************************/

			//We reach the end of a block: we create the block and mark this place as a new start
			IRBlock *newBlock = new IRBlock(previousBlockStart+startAddress, indexInVLIWBinaries+startAddress, section);
			newBlock->sourceStartAddress = previousBlockStartSource + (sectionStartAddress>>2);
			newBlock->sourceEndAddress = indexInMipsBinaries + (sectionStartAddress>>2);
			newBlock->sourceDestination = -1;


			application->addBlock(newBlock, section);


			/******************************************************************************************
			 ******************************  Branch resolution
			 ******************************************************************************************
			 * If we find that there is an unresolved jump in this block, we find the correct location and
			 * solve it if possible (eg. if the destination has already been translated.
			 *
			 ******************************************************************************************/
			unsigned int oneJumpSource = dbtPlateform->unresolvedJumps_src[unresolvedJumpIndex];
			unsigned int oneJumpInitialDestination = dbtPlateform->unresolvedJumps[unresolvedJumpIndex];
			unsigned int oneJumpType = dbtPlateform->unresolvedJumps_type[unresolvedJumpIndex];

			if (newBlock->vliwEndAddress - 2 == oneJumpSource){

				//We save the destination
				newBlock->sourceDestination = oneJumpInitialDestination+(sectionStartAddress>>2);

				unsigned char isAbsolute = ((oneJumpType & 0x7f) != VEX_BR) && ((oneJumpType & 0x7f) != VEX_BRF);
				unsigned int destinationInVLIWFromNewMethod = solveUnresolvedJump(oneJumpInitialDestination+((sectionStartAddress>>2)-mipsStartAddress));
				if (destinationInVLIWFromNewMethod == -1){
					//In this case, the jump cannot be resolved because the destination block is not translated yet.
					//We store information concerning the destination and it will be resolved later

					int numberUnresolvedJumps = unresolvedJumpsArray[0];
					unresolvedJumpsArray[1+numberUnresolvedJumps] = oneJumpInitialDestination+((sectionStartAddress>>2)-mipsStartAddress);
					unresolvedJumpsTypeArray[1+numberUnresolvedJumps] = oneJumpType;
					unresolvedJumpsSourceArray[1+numberUnresolvedJumps] = oneJumpSource;

					unresolvedJumpsArray[0] = numberUnresolvedJumps+1;
				}
				else{
					//The jump can be resolved
					int immediateValue = (isAbsolute) ? (destinationInVLIWFromNewMethod << 2) : ((destinationInVLIWFromNewMethod  - oneJumpSource)<<2);
					writeInt(dbtPlateform->vliwBinaries, 16*(oneJumpSource), oneJumpType + ((immediateValue & 0x7ffff)<<7));

					unsigned int instructionBeforePreviousDestination = readInt(dbtPlateform->vliwBinaries, 16*(destinationInVLIWFromNewMethod-1)+12);
					if (instructionBeforePreviousDestination != 0)
						writeInt(dbtPlateform->vliwBinaries, 16*(oneJumpSource+1)+12, instructionBeforePreviousDestination);
				}

				unresolvedJumpIndex++;

				//We check if the boundary was already marked

				int offsetForDestination = (oneJumpInitialDestination + ((sectionStartAddress>>2) - mipsStartAddress));
				int sectionOfDestination = offsetForDestination>>10;


				if (sectionOfDestination < section){
					bool isDestinationAlreadyMarked = false;
					IRBlock *blockToSplit;

					for (int oneBlockForSucc = 0; oneBlockForSucc<application->numbersBlockInSections[sectionOfDestination]; oneBlockForSucc++){
						IRBlock* blockForSucc = application->blocksInSections[sectionOfDestination][oneBlockForSucc];
						fprintf(stderr, "Looking in block %x\n", blockForSucc->sourceStartAddress);

						if (blockForSucc->sourceStartAddress == newBlock->sourceDestination){
							isDestinationAlreadyMarked = true;
						}
						if (blockForSucc->sourceStartAddress<newBlock->sourceDestination && blockForSucc->sourceEndAddress>newBlock->sourceDestination){
							blockToSplit = blockForSucc;
							fprintf(stderr, "Found the block\n");
						}
					}

					if (!isDestinationAlreadyMarked){
						fprintf(stderr, "Solving block definition for %x\n", newBlock->sourceDestination);


						IRBlock *splittedBlock = new IRBlock(destinationInVLIWFromNewMethod, blockToSplit->vliwEndAddress, sectionOfDestination);
						application->addBlock(splittedBlock, sectionOfDestination);

						//We set metainfo for new block
						splittedBlock->sourceStartAddress = newBlock->sourceDestination;
						splittedBlock->sourceEndAddress = blockToSplit->sourceEndAddress;

						//We set meta info for old block
						blockToSplit->sourceEndAddress = newBlock->sourceDestination;
						blockToSplit->sourceDestination = newBlock->sourceDestination;
						blockToSplit->vliwEndAddress = destinationInVLIWFromNewMethod;
					}

				}

			}



			//If the block is big enough, we profile it
			if (newBlock->vliwEndAddress - newBlock->vliwStartAddress > 7)
				profiler->profileBlock(application->blocksInSections[section][application->numbersBlockInSections[section] - 1]);


			/******************************************************************************************/
			// We update interLoop values
			previousBlockStart = indexInVLIWBinaries;
			previousBlockStartSource = indexInMipsBinaries;

		}


		//We increase counters: both if we are not in an insertion, only the VLIW if we are
		indexInVLIWBinaries++;
		if (!isInsertion)
			indexInMipsBinaries++;
	}

	//We free temporary used data
	free(insertionMap);


}

void buildAdvancedControlFlow(DBTPlateform *platform, IRBlock *startBlock, IRApplication *application){

	IRBlock *blocksToStudy[50];
	int numberBlockToStudy = 1;
	blocksToStudy[0] = startBlock;
	IRBlock *entryBlock = startBlock;
	int indexEntryBlock = 0;

	IRBlock *blockInProcedure[TEMP_BLOCK_STORAGE_SIZE];
	int numberBlockInProcedure = 0;



	while (numberBlockToStudy != 0){

		IRBlock *currentBlock = blocksToStudy[numberBlockToStudy-1];
		numberBlockToStudy--;

		unsigned int endAddress = currentBlock->vliwEndAddress;
		unsigned int jumpInstruction = readInt(platform->vliwBinaries, (endAddress-2)*16);

		fprintf(stderr, "jump is %x\n", jumpInstruction);

		if (currentBlock->nbSucc != -1)
			continue;

		if (numberBlockInProcedure>TEMP_BLOCK_STORAGE_SIZE){
			fprintf(stderr, "Error while building advanced control flow: temporary storage size for blocks is too small and nothing has been implemented to handle this...\n");
			exit(0);
		}

		blockInProcedure[numberBlockInProcedure] = currentBlock;
		numberBlockInProcedure++;


		/******************************************************************************************
		 ******************************  Successor resolution
		 ******************************************************************************************
		 * In this part we will go through all blocks in order to find the object which represent successors of the current block.
		 * This step is currently expensive and may be simplified by creating a map between addresses and blocks or maybe a
		 * dichotomy search function (if they are correctly sorted according to their start address).
		 *
		 ******************************************************************************************/

		//We determine the kind of jump we face
		bool isConditionalBranch = ((jumpInstruction & 0x7f) == VEX_BR) || ((jumpInstruction & 0x7f) == VEX_BRF);
		bool isJump = (jumpInstruction & 0x7f) == VEX_GOTO;
		bool isCall = (jumpInstruction & 0x7f) == VEX_CALL;
		bool isReturn = (jumpInstruction & 0x7f) == VEX_RETURN;
		bool isNothing = ((jumpInstruction & 0x7f) != VEX_CALL) && ((jumpInstruction & 0x7f) != VEX_CALLR) && ((jumpInstruction & 0x7f) != VEX_GOTOR) && ((jumpInstruction & 0x7f) != VEX_STOP);


		//We determine the name of successor(s)
		int successor1, successor2, nbSucc;
		if (isConditionalBranch){
			successor1 = currentBlock->sourceDestination;
			successor2 = currentBlock->sourceEndAddress;
			nbSucc = 2;
			fprintf(stderr, "Looking for %x and %x\n", successor1, successor2);

		}
		else if (isJump){
			if (currentBlock->sourceDestination != -1){
				successor1 = currentBlock->sourceDestination;
				nbSucc = 1;
				fprintf(stderr, "Looking for %x\n", successor1);

			}
		}
		else if (isCall || isNothing){
			successor1 = currentBlock->sourceEndAddress;
			nbSucc = 1;
			fprintf(stderr, "Looking for %x\n", successor1);

		}
		else{
			nbSucc = 0;
		}

		//We find the corresponding block(s)
		if (nbSucc > 0)
			for (int oneSection = 0; oneSection<application->numberOfSections; oneSection++){
				for (int oneBlock = 0; oneBlock < application->numbersBlockInSections[oneSection]; oneBlock++){
					IRBlock *block = application->blocksInSections[oneSection][oneBlock];
					if (block->sourceStartAddress == successor1)
						currentBlock->successor1 = block;
					else if (nbSucc > 1 && block->sourceStartAddress == successor2)
						currentBlock->successor2 = block;
				}
			}


		//We store the result and add the blocks to the list of block to study
		currentBlock->nbSucc = nbSucc;

		if (nbSucc > 0){
			blocksToStudy[numberBlockToStudy] = currentBlock->successor1;
		}
		if (nbSucc > 1){
			blocksToStudy[numberBlockToStudy+1] = currentBlock->successor2;
		}
		numberBlockToStudy += nbSucc;


		//We actualize if needed the entryBlock TODO:check this
		if (entryBlock->vliwStartAddress > currentBlock->vliwStartAddress){
			entryBlock = currentBlock;
			indexEntryBlock = numberBlockInProcedure;
		}

	}


	//We instanciate the procedure
	IRProcedure *procedure = new IRProcedure(entryBlock, numberBlockInProcedure);
	procedure->blocks = (IRBlock**) malloc(numberBlockInProcedure * sizeof(IRBlock*));

	//TODO code a better sort function
	int previousIndex = 0;
	for (int oneBlock = 0; oneBlock<numberBlockInProcedure; oneBlock++){
		int minBlock = 0x1000000;
		int minBlockIndex = 0;
		for (int oneOtherBlock = 0; oneOtherBlock<numberBlockInProcedure; oneOtherBlock++){
			if (blockInProcedure[oneOtherBlock]->sourceStartAddress > previousIndex && blockInProcedure[oneOtherBlock]->sourceStartAddress < minBlock){
				minBlock = blockInProcedure[oneOtherBlock]->sourceStartAddress;
				minBlockIndex = oneOtherBlock;
			}

		}
		procedure->blocks[oneBlock] = blockInProcedure[minBlockIndex];
		previousIndex = minBlock;
	}

	procedure->entryBlock = procedure->blocks[0];
	application->addProcedure(procedure);




	//We create IR for all blocks
	for (int oneBasicBlock=0; oneBasicBlock<procedure->nbBlock; oneBasicBlock++){
		IRBlock *block = procedure->blocks[oneBasicBlock];
		if (block->nbInstr == 0){

			int globalVariableCounter = 288;

			for (int oneGlobalVariable = 0; oneGlobalVariable < 64; oneGlobalVariable++)
				platform->globalVariables[oneGlobalVariable] = 256 + oneGlobalVariable;

			int originalScheduleSize = block->vliwEndAddress - block->vliwStartAddress- 1;



			int blockSize = irGenerator(platform, block->vliwStartAddress, originalScheduleSize, globalVariableCounter);



			fprintf(stderr, "analysis returned a block of %d instr for block from %d to %d (size %d)\n", blockSize, block->vliwStartAddress, block->vliwEndAddress, originalScheduleSize);
			block->instructions = (uint32*) malloc(blockSize*4*sizeof(uint32));
			for (int oneBytecodeInstr = 0; oneBytecodeInstr<blockSize; oneBytecodeInstr++){
				block->instructions[4*oneBytecodeInstr + 0] = readInt(platform->bytecode, 16*oneBytecodeInstr + 0);
				block->instructions[4*oneBytecodeInstr + 1] = readInt(platform->bytecode, 16*oneBytecodeInstr + 4);
				block->instructions[4*oneBytecodeInstr + 2] = readInt(platform->bytecode, 16*oneBytecodeInstr + 8);
				block->instructions[4*oneBytecodeInstr + 3] = readInt(platform->bytecode, 16*oneBytecodeInstr + 12);
			}

			//We check if we find a jump as last instruction
			char opcodeOfLastInstr = getOpcode(block->instructions, blockSize-1);
			if ((opcodeOfLastInstr >> 4) == 2 && opcodeOfLastInstr != VEX_MOVI && opcodeOfLastInstr != VEX_SETCOND && opcodeOfLastInstr != VEX_SETCONDF){
				block->jumpID = blockSize-1;
			}

			block->nbInstr = blockSize;
			block->blockState = IRBLOCK_PROC;
		}
	}

	procedure->print();
	fprintf(stderr, "Procedure entry point was %d\n", procedure->entryBlock->vliwStartAddress);

	fprintf(stderr, "Analysis done ! \n");

}
