/*
	This file is part of cpp-ethereum.

	cpp-ethereum is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	cpp-ethereum is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with cpp-ethereum.  If not, see <http://www.gnu.org/licenses/>.
*/
/** @file MixClient.cpp
 * @author Arkadiy Paronyan arkadiy@ethdev.com
 * @date 2015
 * Ethereum IDE client.
 */

#include <vector>
#include <libdevcore/Exceptions.h>
#include <libethereum/CanonBlockChain.h>
#include <libethereum/Transaction.h>
#include <libethereum/Executive.h>
#include <libethereum/ExtVM.h>
#include <libethereum/BlockChain.h>
#include <libevm/VM.h>

#include "Exceptions.h"
#include "MixClient.h"

using namespace dev;
using namespace dev::eth;

namespace dev
{
namespace mix
{

Secret const c_defaultUserAccountSecret = Secret("cb73d9408c4720e230387d956eb0f829d8a4dd2c1055f96257167e14e7169074");
u256 const c_mixGenesisDifficulty = c_minimumDifficulty; //TODO: make it lower for Mix somehow

bytes MixBlockChain::createGenesisBlock(h256 _stateRoot)
{
	RLPStream block(3);
	block.appendList(15)
		<< h256() << EmptyListSHA3 << h160() << _stateRoot << EmptyTrie << EmptyTrie
		<< LogBloom() << c_mixGenesisDifficulty << 0 << c_genesisGasLimit << 0 << (unsigned)0
		<< std::string() << h256() << h64(u64(42));
	block.appendRaw(RLPEmptyList);
	block.appendRaw(RLPEmptyList);
	return block.out();
}

MixClient::MixClient(std::string const& _dbPath):
	m_dbPath(_dbPath), m_miningThreads(0)
{
	std::map<Secret, u256> account;
	account.insert(std::make_pair(c_defaultUserAccountSecret, 1000000 * ether));
	resetState(account);
}

MixClient::~MixClient()
{
}

void MixClient::resetState(std::map<Secret, u256> _accounts)
{
	WriteGuard l(x_state);
	Guard fl(x_filtersWatches);
	m_filters.clear();
	m_watches.clear();

	m_stateDB = OverlayDB();
	SecureTrieDB<Address, MemoryDB> accountState(&m_stateDB);
	accountState.init();

	m_userAccounts.clear();
	std::map<Address, Account> genesisState;
	for (auto account: _accounts)
	{
		KeyPair a = KeyPair(account.first);
		m_userAccounts.push_back(a);
		genesisState.insert(std::make_pair(a.address(), Account(account.second, Account::NormalCreation)));
	}

	dev::eth::commit(genesisState, static_cast<MemoryDB&>(m_stateDB), accountState);
	h256 stateRoot = accountState.root();
	m_bc.reset();
	m_bc.reset(new MixBlockChain(m_dbPath, stateRoot));
	m_state = eth::State(m_stateDB, BaseState::PreExisting, genesisState.begin()->first);
	m_state.sync(bc());
	m_startState = m_state;
	WriteGuard lx(x_executions);
	m_executions.clear();
}

Transaction MixClient::replaceGas(Transaction const& _t, Secret const& _secret, u256 const& _gas)
{
	if (_t.isCreation())
		return Transaction(_t.value(), _t.gasPrice(), _gas, _t.data(), _t.nonce(), _secret);
	else
		return Transaction(_t.value(), _t.gasPrice(), _gas, _t.receiveAddress(), _t.data(), _t.nonce(), _secret);
}

void MixClient::executeTransaction(Transaction const& _t, State& _state, bool _call, bool _gasAuto, Secret const& _secret)
{
	Transaction t = _gasAuto ? replaceGas(_t, _secret, m_state.gasLimitRemaining()) : _t;
	bytes rlp = t.rlp();

	// do debugging run first
	LastHashes lastHashes(256);
	lastHashes[0] = bc().numberHash(bc().number());
	for (unsigned i = 1; i < 256; ++i)
		lastHashes[i] = lastHashes[i - 1] ? bc().details(lastHashes[i - 1]).parent : h256();

	State execState = _state;
	Executive execution(execState, lastHashes, 0);
	execution.initialize(&rlp);
	execution.execute();
	std::vector<MachineState> machineStates;
	std::vector<unsigned> levels;
	std::vector<MachineCode> codes;
	std::map<bytes const*, unsigned> codeIndexes;
	std::vector<bytes> data;
	std::map<bytesConstRef const*, unsigned> dataIndexes;
	bytes const* lastCode = nullptr;
	bytesConstRef const* lastData = nullptr;
	unsigned codeIndex = 0;
	unsigned dataIndex = 0;
	auto onOp = [&](uint64_t steps, Instruction inst, dev::bigint newMemSize, dev::bigint gasCost, void* voidVM, void const* voidExt)
	{
		VM& vm = *static_cast<VM*>(voidVM);
		ExtVM const& ext = *static_cast<ExtVM const*>(voidExt);
		if (lastCode == nullptr || lastCode != &ext.code)
		{
			auto const& iter = codeIndexes.find(&ext.code);
			if (iter != codeIndexes.end())
				codeIndex = iter->second;
			else
			{
				codeIndex = codes.size();
				codes.push_back(MachineCode({ext.myAddress, ext.code}));
				codeIndexes[&ext.code] = codeIndex;
			}
			lastCode = &ext.code;
		}

		if (lastData == nullptr || lastData != &ext.data)
		{
			auto const& iter = dataIndexes.find(&ext.data);
			if (iter != dataIndexes.end())
				dataIndex = iter->second;
			else
			{
				dataIndex = data.size();
				data.push_back(ext.data.toBytes());
				dataIndexes[&ext.data] = dataIndex;
			}
			lastData = &ext.data;
		}

		if (levels.size() < ext.depth)
			levels.push_back(machineStates.size() - 1);
		else
			levels.resize(ext.depth);

		machineStates.emplace_back(MachineState({steps, vm.curPC(), inst, newMemSize, vm.gas(),
									  vm.stack(), vm.memory(), gasCost, ext.state().storage(ext.myAddress), levels, codeIndex, dataIndex}));
	};

	execution.go(onOp);
	execution.finalize();
	dev::eth::ExecutionResult er = execution.executionResult();

	switch (er.excepted)
	{
		case TransactionException::None:
			break;
		case TransactionException::NotEnoughCash:
			BOOST_THROW_EXCEPTION(Exception() << errinfo_comment("Insufficient balance for contract deployment"));
		case TransactionException::OutOfGasBase:
		case TransactionException::OutOfGas:
			BOOST_THROW_EXCEPTION(OutOfGas() << errinfo_comment("Not enough gas"));
		case TransactionException::BlockGasLimitReached:
			BOOST_THROW_EXCEPTION(OutOfGas() << errinfo_comment("Block gas limit reached"));
		case TransactionException::OutOfStack:
			BOOST_THROW_EXCEPTION(Exception() << errinfo_comment("Out of stack"));
		case TransactionException::StackUnderflow:
			BOOST_THROW_EXCEPTION(Exception() << errinfo_comment("Stack underflow"));
		//these should not happen in mix
		case TransactionException::Unknown:
		case TransactionException::BadInstruction:
		case TransactionException::BadJumpDestination:
		case TransactionException::InvalidSignature:
		case TransactionException::InvalidNonce:
			BOOST_THROW_EXCEPTION(Exception() << errinfo_comment("Internal execution error"));
	};

	ExecutionResult d;
	d.result = execution.executionResult();
	d.machineStates = machineStates;
	d.executionCode = std::move(codes);
	d.transactionData = std::move(data);
	d.address = _t.receiveAddress();
	d.sender = _t.sender();
	d.value = _t.value();
	d.gasUsed = er.gasUsed + er.gasRefunded;
	if (_t.isCreation())
		d.contractAddress = right160(sha3(rlpList(_t.sender(), _t.nonce())));
	if (!_call)
		d.transactionIndex = m_state.pending().size();
	d.executonIndex = m_executions.size();

	// execute on a state
	if (!_call)
	{
		t = _gasAuto ? replaceGas(_t, _secret, d.gasUsed) : _t;
		er =_state.execute(lastHashes, t);
		if (t.isCreation() && _state.code(d.contractAddress).empty())
			BOOST_THROW_EXCEPTION(OutOfGas() << errinfo_comment("Not enough gas for contract deployment"));
		d.gasUsed = er.gasUsed + er.gasRefunded + er.gasForDeposit;
		// collect watches
		h256Set changed;
		Guard l(x_filtersWatches);
		for (std::pair<h256 const, eth::InstalledFilter>& i: m_filters)
			if ((unsigned)i.second.filter.latest() > bc().number())
			{
				// acceptable number.
				auto m = i.second.filter.matches(_state.receipt(_state.pending().size() - 1));
				if (m.size())
				{
					// filter catches them
					for (LogEntry const& l: m)
						i.second.changes.push_back(LocalisedLogEntry(l, bc().number() + 1));
					changed.insert(i.first);
				}
			}
		changed.insert(dev::eth::PendingChangedFilter);
		noteChanged(changed);
	}
	WriteGuard l(x_executions);
	m_executions.emplace_back(std::move(d));
}

void MixClient::mine()
{
	WriteGuard l(x_state);
	m_state.commitToMine(bc());
	m_state.completeMine();
	bc().import(m_state.blockData(), m_stateDB, Aversion::AvoidOldBlocks, ImportRequirements::ValidNonce | ImportRequirements::DontHave);
	m_state.sync(bc());
	m_startState = m_state;
	h256Set changed { dev::eth::PendingChangedFilter, dev::eth::ChainChangedFilter };
	noteChanged(changed);
}

ExecutionResult MixClient::lastExecution() const
{
	ReadGuard l(x_executions);
	return m_executions.empty() ? ExecutionResult() : m_executions.back();
}

ExecutionResult MixClient::execution(unsigned _index) const
{
	ReadGuard l(x_executions);
	return m_executions.at(_index);
}

State MixClient::asOf(h256 const& _block) const
{
	ReadGuard l(x_state);
	return State(m_stateDB, bc(), _block);
}

void MixClient::submitTransaction(Secret _secret, u256 _value, Address _dest, bytes const& _data, u256 _gas, u256 _gasPrice, bool _gasAuto)
{
	WriteGuard l(x_state);
	u256 n = m_state.transactionsFrom(toAddress(_secret));
	Transaction t(_value, _gasPrice, _gas, _dest, _data, n, _secret);
	executeTransaction(t, m_state, false, _gasAuto, _secret);
}

Address MixClient::submitTransaction(Secret _secret, u256 _endowment, bytes const& _init, u256 _gas, u256 _gasPrice, bool _gasAuto)
{
	WriteGuard l(x_state);
	u256 n = m_state.transactionsFrom(toAddress(_secret));
	eth::Transaction t(_endowment, _gasPrice, _gas, _init, n, _secret);
	executeTransaction(t, m_state, false, _gasAuto, _secret);
	Address address = right160(sha3(rlpList(t.sender(), t.nonce())));
	return address;
}

dev::eth::ExecutionResult MixClient::call(Secret _secret, u256 _value, Address _dest, bytes const& _data, u256 _gas, u256 _gasPrice, BlockNumber _blockNumber, bool _gasAuto, FudgeFactor _ff)
{
	(void)_blockNumber;
	Address a = toAddress(_secret);
	State temp = asOf(eth::PendingBlock);
	u256 n = temp.transactionsFrom(a);
	Transaction t(_value, _gasPrice, _gas, _dest, _data, n, _secret);
	if (_ff == FudgeFactor::Lenient)
		temp.addBalance(a, (u256)(t.gasRequired() * t.gasPrice() + t.value()));
	bytes rlp = t.rlp();
	WriteGuard lw(x_state); //TODO: lock is required only for last execution state
	executeTransaction(t, temp, true, _gasAuto, _secret);
	return lastExecution().result;
}

void MixClient::submitTransaction(Secret _secret, u256 _value, Address _dest, bytes const& _data, u256 _gas, u256 _gasPrice)
{
	submitTransaction(_secret, _value, _dest, _data, _gas, _gasPrice, false);
}

Address MixClient::submitTransaction(Secret _secret, u256 _endowment, bytes const& _init, u256 _gas, u256 _gasPrice)
{
	return submitTransaction(_secret, _endowment, _init, _gas, _gasPrice, false);
}

dev::eth::ExecutionResult MixClient::call(Secret _secret, u256 _value, Address _dest, bytes const& _data, u256 _gas, u256 _gasPrice, BlockNumber _blockNumber, eth::FudgeFactor _ff)
{
	return call(_secret, _value, _dest, _data, _gas, _gasPrice, _blockNumber, false, _ff);
}

dev::eth::ExecutionResult MixClient::create(Secret _secret, u256 _value, bytes const& _data, u256 _gas, u256 _gasPrice, BlockNumber _blockNumber, eth::FudgeFactor _ff)
{
	(void)_blockNumber;
	u256 n;
	Address a = toAddress(_secret);
	State temp;
	{
		ReadGuard lr(x_state);
		temp = asOf(eth::PendingBlock);
		n = temp.transactionsFrom(a);
	}
	Transaction t(_value, _gasPrice, _gas, _data, n, _secret);
	if (_ff == FudgeFactor::Lenient)
		temp.addBalance(a, (u256)(t.gasRequired() * t.gasPrice() + t.value()));
	bytes rlp = t.rlp();
	WriteGuard lw(x_state); //TODO: lock is required only for last execution state
	executeTransaction(t, temp, true, false, _secret);
	return lastExecution().result;
}

void MixClient::noteChanged(h256Set const& _filters)
{
	for (auto& i: m_watches)
		if (_filters.count(i.second.id))
		{
			if (m_filters.count(i.second.id))
				i.second.changes += m_filters.at(i.second.id).changes;
			else
				i.second.changes.push_back(LocalisedLogEntry(SpecialLogEntry, 0));
		}
	for (auto& i: m_filters)
		i.second.changes.clear();
}

eth::BlockInfo MixClient::blockInfo() const
{
	ReadGuard l(x_state);
	return BlockInfo(bc().block());
}

void MixClient::setAddress(Address _us)
{
	WriteGuard l(x_state);
	m_state.setAddress(_us);
}

void MixClient::setMiningThreads(unsigned _threads)
{
	m_miningThreads = _threads;
}

unsigned MixClient::miningThreads() const
{
	return m_miningThreads;
}

void MixClient::startMining()
{
	//no-op
}

void MixClient::stopMining()
{
	//no-op
}

bool MixClient::isMining()
{
	return false;
}

eth::MineProgress MixClient::miningProgress() const
{
	return eth::MineProgress();
}

}
}
