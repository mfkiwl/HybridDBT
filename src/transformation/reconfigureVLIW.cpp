/*
 * reconfigureVLIW.cpp
 *
 *  Created on: 9 févr. 2017
 *      Author: simon
 */


#include <isa/vexISA.h>
#include <transformation/reconfigureVLIW.h>

unsigned int schedulerConfigurations[16] = {0x00005e00,0x0000546c,0x00005ec4,0x046c50c4,0x00005ecc,0x040c5ec4,0,0,
											0x00005e64,0x44605e04,0x00005e6c,0x40605ec4,0x006c5ec4,0x446c5ec4,0,0};
char validConfigurations[12] = {0, 1, 2, 3, 4, 5, 8, 9, 10, 11, 12 ,13};

char getIssueWidth(char configuration){
	//This function returns the issue width of each configuration code

	//We extract variables
	char regCode = (configuration >> 4) & 0x1;
	char memCode = (configuration >> 3) & 0x1;
	char multCode = (configuration >> 1) & 0x3;
	char boostCode = configuration & 0x1;

	//Sum resources of mem and mult and round to the upper even number (this is the or)
	char issueWidth = (memCode + multCode + 2);

	if (issueWidth & 0x1)
		issueWidth++;

	//If boost code then it is the version with increased issue width
	if (boostCode)
		issueWidth += 2;

	return issueWidth;

}

unsigned int getConfigurationForScheduler(char configuration){
	return schedulerConfigurations[configuration % 16];
}

unsigned int getReconfigurationInstruction(char configuration){
	unsigned int schedulerConf = getConfigurationForScheduler(configuration);

	char mux6 = ((schedulerConf>>8) & 0xf) == 4;
	char mux7 = ((schedulerConf>>4) & 0xf) == 2;
	char mux8 = ((schedulerConf>>0) & 0xf) == 8;

	char activations[8];
	activations[0] = ((schedulerConf>>(4*3)) & 0xf) != 0;
	activations[1] = ((schedulerConf>>(4*2)) & 0xf) != 0 && !mux6;
	activations[2] = ((schedulerConf>>(4*1)) & 0xf) != 0 && !mux7;
	activations[3] = ((schedulerConf>>(4*0)) & 0xf) != 0 && !mux8;
	activations[4] = ((schedulerConf>>(4*7)) & 0xf) != 0;
	activations[5] = ((schedulerConf>>(4*6)) & 0xf) != 0 || mux6;
	activations[6] = ((schedulerConf>>(4*5)) & 0xf) != 0 || mux7;
	activations[7] = ((schedulerConf>>(4*4)) & 0xf) != 0 || mux8;

	char issueWidth = getIssueWidth(configuration);
	char regFileControlBit = configuration>>4;

	unsigned int immediateValue = activations[0]
								+ (activations[1]<<1)
								+ (activations[2]<<2)
								+ (activations[3]<<3)
								+ (activations[4]<<4)
								+ (activations[5]<<5)
								+ (activations[6]<<6)
								+ (activations[7]<<7)

								+ (mux6<<8)
								+ (mux7<<9)
								+ (mux8<<10)

								+ (issueWidth<<11)
								+ (regFileControlBit<<15);

	return assembleIInstruction(VEX_RECONFFS, immediateValue, configuration);
}

char getNbMem(char configuration){
	char memCode = (configuration >> 3) & 0x1;
	return memCode + 1;
}

char getNbMult(char configuration){
	char multCode = (configuration >> 1) & 0x3;
	return multCode + 1;
}

float getPowerConsumption(char configuration){

	char nbMem = getNbMem(configuration);
	char nbMult = getNbMult(configuration);
	char issueWidh = getIssueWidth(configuration);

	float coef = 1;
	if (configuration>=16)
		coef = 1.5;
	float powerConsumption = coef*issueWidh + (nbMem*2) + (nbMult);


	return powerConsumption;
}

void setVLIWConfiguration(VexSimulator *simulator, char configuration){
	unsigned int schedulerConf = getConfigurationForScheduler(configuration);

	simulator->muxValues[0] = ((schedulerConf>>8) & 0xf) == 4;
	simulator->muxValues[1] = ((schedulerConf>>4) & 0xf) == 2;
	simulator->muxValues[2] = ((schedulerConf>>0) & 0xf) == 8;

	simulator->unitActivation[0] = ((schedulerConf>>(4*3)) & 0xf) != 0;
	simulator->unitActivation[1] = ((schedulerConf>>(4*2)) & 0xf) != 0 && !simulator->muxValues[0];
	simulator->unitActivation[2] = ((schedulerConf>>(4*1)) & 0xf) != 0 && !simulator->muxValues[1];
	simulator->unitActivation[3] = ((schedulerConf>>(4*0)) & 0xf) != 0 && !simulator->muxValues[2];
	simulator->unitActivation[4] = ((schedulerConf>>(4*7)) & 0xf) != 0;
	simulator->unitActivation[5] = ((schedulerConf>>(4*6)) & 0xf) != 0 || simulator->muxValues[0];
	simulator->unitActivation[6] = ((schedulerConf>>(4*5)) & 0xf) != 0 || simulator->muxValues[1];
	simulator->unitActivation[7] = ((schedulerConf>>(4*4)) & 0xf) != 0 || simulator->muxValues[2];

	simulator->issueWidth = getIssueWidth(configuration);
	char regFileControlBit = configuration>>4;

	simulator->currentConfig = configuration;

}

void changeConfiguration(IRProcedure *procedure){


	for (int oneValidConfiguration = 0; oneValidConfiguration < 12; oneValidConfiguration++){
		char oneConfiguration = validConfigurations[oneValidConfiguration];
		if (procedure->configurationScores[oneConfiguration] == 0){
			fprintf(stderr, "Changing configuration from %d to %d\n", procedure->configuration, oneConfiguration);
			procedure->previousConfiguration = procedure->configuration;
			procedure->configuration = oneConfiguration;
			return;
		}
	}

	procedure->state = 1;

}

int computeScore(IRProcedure *procedure){
	float result = 0;
	for (int oneBlock = 0; oneBlock<procedure->nbBlock; oneBlock++){
		IRBlock *block = procedure->blocks[oneBlock];
		result += block->vliwEndAddress - block->vliwStartAddress;
	}
	if (getIssueWidth(procedure->configuration) > 4)
		result = result/2;

	result = 100000 / (result*getPowerConsumption(procedure->configuration));

	fprintf(stderr, "Configuration with %x is %f\n", procedure->configuration, result);
	return (int) result;
}

int suggestConfiguration(IRProcedure *originalProc, IRProcedure *newlyScheduledProc){
	int nbInstr = getNbInstr(originalProc);
	int nbMult = getNbInstr(originalProc, 3);
	int nbMem = getNbInstr(originalProc, 1);


	int size = 0;
	for (int oneBlock = 0; oneBlock<newlyScheduledProc->nbBlock; oneBlock++){
		IRBlock *block = newlyScheduledProc->blocks[oneBlock];
		size += block->vliwEndAddress - block->vliwStartAddress;
	}
	if (getIssueWidth(newlyScheduledProc->configuration) > 4)
		size = size / 2;

	int ressourceMult = getNbMult(newlyScheduledProc->configuration);
	int ressourceMem = getNbMult(newlyScheduledProc->configuration);
	int ressourceInstr = getIssueWidth(newlyScheduledProc->configuration);

	fprintf(stderr, "schedule size is %d procedure has %d insructions, %d mem, %d mult\n", size, nbInstr,nbMem, nbMult);
	float scoreMult = 1.0 * nbMult / (size * ressourceMult);
	float scoreMem = 1.0 * nbMem / (size * ressourceMem);
	float scoreSimple = 1.0 * nbInstr / (size * ressourceInstr);

	fprintf(stderr, "Scores for suggestion are %f %f %f\n", scoreMult, scoreMem, scoreSimple);

}


