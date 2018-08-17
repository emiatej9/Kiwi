#include "stdafx.h"
#include "KNLangModel.h"

using namespace std;

KNLangModel::KNLangModel(size_t _orderN) : orderN(_orderN)
{
	nodes.emplace_back();
}

void KNLangModel::prepareCapacity(size_t minFreeSize)
{
	if (nodes.capacity() < nodes.size() + minFreeSize)
	{
		nodes.reserve(max(nodes.size() + minFreeSize, nodes.capacity() + nodes.capacity() / 2));
	}
}

void KNLangModel::trainSequence(const WID * seq, size_t len)
{
	prepareCapacity(len * orderN);
	for (size_t i = 0; i < len; ++i)
	{
		nodes[0].increaseCount(seq + i, seq + min(i + orderN, len), orderN - 1, nodesAlloc);
	}
	vocabSize = max((size_t)*max_element(seq, seq + len) + 1, vocabSize);
}

void KNLangModel::calcDiscountedValue(size_t order, const vector<uint32_t>& cntNodes)
{
	// modified unigram probability
	if (order == 1)
	{
		size_t cntBigram = 0;
		vector<WID> cnt;
		cnt.resize(vocabSize);
		for (auto& node : nodes)
		{
			if (node.depth != 1) continue;
			for (auto&& p : node)
			{
				cnt[p.first]++;
				cntBigram++;
			}
		}

		for (auto& p : cnt)
		{
			auto* n = nodes[0].getNext(&p - &cnt[0]);
			if (n) n->ll = p / (float)cntBigram;
		}
		return;
	}

	size_t numCount[4] = { 0, };
	map<vector<WID>, size_t> discntNum[3];
	if (order == orderN) for (auto& node : nodes)
	{
		if (node.depth != orderN - 1) continue;
		for (auto&& p : node)
		{
			// in the leaf node
			uint32_t leafCnt = p.second - &node;
			if (leafCnt <= 4) numCount[leafCnt - 1]++;
		}
	}
	else for (auto& node : nodes)
	{
		if (node.depth != order) continue;
		auto cnt = cntNodes[&node - &nodes[0]];
		if (cnt <= 4) numCount[cnt - 1]++;
	}

	// calculating discount value
	float y = numCount[0] / (numCount[0] + 2.f * numCount[1]);
	float discntValue[3];
	for (size_t i = 0; i < 3; ++i)
	{
		discntValue[i] = numCount[i] ? (i + 1.f - (i + 2.f) * y * numCount[i + 1] / numCount[i]) : 0;
		assert(discntValue[i] >= 0);
	}

	// calculating gamma
	for (auto& node : nodes)
	{
		if (node.depth != order - 1) continue;
		size_t discntNum[3] = { 0, };
		for (auto&& p : node)
		{
			uint32_t cnt;
			// in the leaf node
			if (order == orderN) cnt = p.second - &node;
			else cnt = cntNodes[p.second - &nodes[0]];
			discntNum[min(cnt, 3u) - 1]++;
		}
		node.gamma = 0;
		for (size_t i = 0; i < 3; ++i) node.gamma += discntValue[i] * discntNum[i];
		node.gamma /= cntNodes[&node - &nodes[0]];
	}

	// applying smooth probability
	if (order == orderN) for (auto& node : nodes)
	{
		if (node.depth != orderN - 1) continue;
		for (auto&& p : node)
		{
			// in the leaf node
			uint32_t leafCnt = p.second - &node;
			float ll = (leafCnt - discntValue[min(leafCnt, 3u) - 1]) / cntNodes[&node - &nodes[0]];
			ll += node.gamma * node.getLower()->getNext(p.first)->ll;
			node.setLL(p.first, ll);
		}
	}
	else for (auto& node : nodes)
	{
		if (node.depth != order) continue;
		auto cnt = cntNodes[&node - &nodes[0]];
		node.ll = (cnt - discntValue[min(cnt, 3u) - 1]) / cntNodes[node.getParent() - &nodes[0]];
		node.ll += node.getParent()->gamma * node.getLower()->ll;
	}
}

void KNLangModel::optimize()
{
	{
		vector<uint32_t> cntNodes(nodes.size());
		std::transform(nodes.begin(), nodes.end(), cntNodes.begin(), [](const Node& n)
		{
			return n.count;
		});
		for (size_t i = 1; i <= orderN; ++i)
		{
			calcDiscountedValue(i, cntNodes);
		}
	}

	// bake likelihoods to log
	nodes[0].ll = 1;
	for (auto& node : nodes)
	{
		node.ll = log(node.ll);
		node.gamma = log(node.gamma);
		if (node.depth == orderN - 1)
		{
			for (auto&& p : node)
			{
				uint32_t t = p.second - &node;
				node.setLL(p.first, log(*(float*)&t));
			}
		}
		node.optimize();
	}
}

vector<float> KNLangModel::predictNext(const WID * history, size_t len) const
{
	vector<float> prob(vocabSize);
	const Node* n = nullptr;
	for (size_t i = max(len, orderN - 1) - orderN + 1; i < len && !(n = nodes[0].getFromBaked(history + i, history + len)); ++i);
	if (!n) n = &nodes[0];
	for (size_t i = 0; i < vocabSize; ++i)
	{
		prob[i] = n->getLL(i, orderN - 1);
	}
	return prob;
}

float KNLangModel::evaluateLL(const WID * seq, size_t len) const
{
	const Node* n = nullptr;
	for (size_t i = max(len - 1, orderN - 1) - orderN + 1; i < len - 1 && !(n = nodes[0].getFromBaked(seq + i, seq + len - 1)); ++i);
	if (!n) n = &nodes[0];
	return n->getLL(seq[len - 1], orderN - 1);
}

float KNLangModel::evaluateLLSent(const WID * seq, size_t len) const
{
	float sum = 0;
	for (size_t i = 2; i <= len; ++i)
	{
		sum += evaluateLL(seq, i);
	}
	return sum;
}
