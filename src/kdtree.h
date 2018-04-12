#pragma once

#include "types.h"
#include "cgal_types.h"
#include "taskscheduler.h"

#include <set>
#include <queue>
#include <cmath> //for log2
#include <QDebug>

class kdTree
{
	//task struct, task* megy a taskmanagerbe - h�tr�ny nek�nk kell megsz�ntetni, el�ny: elt�nik a start �s a length
	struct node;
	typedef std::pair<float, node*> DistanceTuple;

	struct LargestOnTop
	{
		bool operator()(const DistanceTuple &a, const DistanceTuple &b) const { return a.first < b.first; }
	};

public:

	typedef std::priority_queue<DistanceTuple, std::vector<DistanceTuple>, LargestOnTop > kNearest_queue;

	struct nearest_node
	{
		node* neighb;
		float distance;
		nearest_node(node* n = NULL, const float& d = FLT_MAX) : neighb(n), distance(d) {}
	};

private:
	struct node
	{
		node* left, *right;
		node* parent;

		int depth, length;
		Box box;			//TODO: just pointer and make an other node for boxes -> compute_bounding_boxes

		point_uv_tuple* point;	//pointer to node val
		point_uv_tuple* start;	//pointer to starter point in the vector

		nearest_node nn;
		kNearest_queue knn;
		//float spacing;		//TODO: some functions for compute this, i.e. centroid , compute_average_spacing()

		node(node* p, point_uv_tuple* s, const int& d, const int& lg, Box b, point_uv_tuple* po = NULL, node* l = NULL, node* r = NULL, const nearest_node& _nn = nearest_node())
			: parent(p), start(s), depth(d), length(lg), box(b), point(po), left(l), right(r), nn(_nn) {}
	};

	typedef typename node* kdnode_ptr;

public:
	kdTree(const int& ma, const int& thr_c) :
		root(NULL), _draw(NULL),
		stopBuildDepth(STOPLEVEL),
		kn(1),
		sort_time(duration_f(0.0f)), calc_time(milliseconds(0))
	{
		t_schedular = new TaskSchedular<kdTree, node>();
		changeThreadCapacity(thr_c);
	}

	~kdTree() {
		release();
		delete t_schedular;
	}

	void build(std::vector<point_uv_tuple>& data, const Box& box)
	{
		calc_time = milliseconds(0);
		sort_time = duration_f(0.0f);

		root = new node(NULL, &data[0], 0, data.size(), box, NULL, NULL, NULL);	//TODO CHECK DATA
		_draw = root;

		//TODO: test the performance
		if (data.size() > CLOUDRECURSIVEPOINTCAP && t_schedular->getCloudThreadCapacity() >= 2)
			buildWithAutomech();
		else
			buildRecursive(root);		//with few points the simple recursion is faster than syncing with main thread

		if (PRINT) inOrderPrint(root, 0);
	}

	void release() 
	{
		_draw = NULL;
		if (root) releaseNode(root);
	}

	//REFACTOR
	//EZ M�S DE TODO nem tudom nem megcsin�lni, mert nem akarok adatot m�solni
	//nem tudom kiszedni t�bb sz�lon csak lockfree vel, de annak van korl�tja
	//viszont meg kell csin�lni, ha nem akarok minden pontot kirajzolni
	// - -> 1x v�gig kell mennem rekurz�van a kapott f�n, ha 1 volt az accuracy ha nem

	ProcessReturns build_process(std::vector<point_uv_tuple>& input, const Box& box) 
	{
		time_p t1 = hrclock::now();
		time_p t2;

		build(input, box);
		if (!root) return UNDEFINED_ERROR;		//TODO

		//----------------------------------------
		t = 0;
		//addNeighbours();
		//addKNeighbours(input);

		//----------------------------------------
		t2 = hrclock::now();
		calc_time = boost::chrono::duration_cast<milliseconds>(t2 - t1);

		return PROCESS_DONE;
	}

	//------------------------------
	inline void changeThreadCapacity(const unsigned int& tc);

	inline void getTimes(float& join, float& sort, float& all) const;
	unsigned int getThreadCapacity() const { return t_schedular->getCloudThreadCapacity(); }

	void goLeftNode() { if (_draw && _draw->left)	_draw = _draw->left; }
	void goRightNode() { if (_draw && _draw->right)	_draw = _draw->right; }
	void goParentNode() { if (_draw && _draw->parent) _draw = _draw->parent; }

	kdnode_ptr getRootPtr() const { return root; }
	GLPaintFormat drawNode(/*const unsigned int&*/);

	//--------------------------------------------------------------

	inline void nearest_p(Point *query, kdnode_ptr currentNode, nearest_node& best);

	inline void knearest_p(Point *query, node* currentNode, kNearest_queue& best);

	void nearestAll(node* query, bool stop = false, const int& stopDepth = 0)
	{
		//test = 0;
		nearestRecursive(query);
		//t++;

		if (stop && stopDepth == query->depth + 1)
		{
			if (query->left)   t_schedular->addTask(query->left);
			if (query->right)  t_schedular->addTask(query->right);
			return;
		}
		else
		{
			if (query->left) nearestAll(query->left);
			if (query->right) nearestAll(query->right);
		}
	}

	void knearestAll(node* query, bool stop = false, const int& stopDepth = 0)
	{
		//test = 0;
		kNearestRecursive(query);
		//t++;

		if (stop && stopDepth == query->depth + 1)
		{
			if (query->left)   t_schedular->addTask(query->left);
			if (query->right)  t_schedular->addTask(query->right);
			return;
		}
		else
		{
			if (query->left) knearestAll(query->left);
			if (query->right) knearestAll(query->right);
		}
	}

	//------------------------------
	inline void inOrderPrint(node* r, const int& level);

	void set_kn(const unsigned int& k)
	{
		kn = k;
	}

protected:
	//Tree buildings
	inline void buildWithAutomech();
	inline void buildRecursive(node* r, bool stop = false, const int& level = 0);
	inline void sortPartVector(const int& axis, point_uv_tuple* c, const int& length);

	//----------------
	// Neighbours calculators
	void addNeighbours(const std::vector<point_uv_tuple>& data)
	{
		if (data.size() > CLOUDRECURSIVEPOINTCAP && t_schedular->getCloudThreadCapacity() >= 2)
		{
			if (root->depth != stopBuildDepth) nearestAll(root, true, stopBuildDepth);
			else t_schedular->addTask(root);

			while (!t_schedular->isEmptyTask())
				t_schedular->addSubscribeShit(this, &kdTree::nearestAll);

			t_schedular->joinAll();		//wait our threads
		}
		else
			nearestAll(root);
	}

	void addKNeighbours(const std::vector<point_uv_tuple>& data)
	{
		if (data.size() > CLOUDRECURSIVEPOINTCAP && t_schedular->getCloudThreadCapacity() >= 2)
		{
			if (root->depth != stopBuildDepth) knearestAll(root, true, stopBuildDepth);
			else t_schedular->addTask(root);

			while (!t_schedular->isEmptyTask())
				t_schedular->addSubscribeShit(this, &kdTree::knearestAll);

			t_schedular->joinAll();		//wait our threads
		}
		else
			knearestAll(root);
	}

	void nearestRecursive(node* query) {
		if (!root) {
			return;
		}
		nearest(query->point, root, query->nn);
	}

	void kNearestRecursive(node* query) {
		if (!root) {
			return;
		}
		knearest(query->point, root, query->knn);
	}

	inline void nearest(point_uv_tuple *query, node* currentNode, nearest_node& best);

	inline void knearest(point_uv_tuple *query, node* currentNode, kNearest_queue& best);

	//----------------
	//Others
	inline void releaseNode(node*);

	inline void getActivePoints(std::vector<point_uv_tuple>& active_points, node* r = NULL);

private:
	kdnode_ptr root;
	kdnode_ptr _draw;

	unsigned int stopBuildDepth;
	TaskSchedular<kdTree, node>* t_schedular;
											
	int kn;										//TODO: better name?? k nearest neighbours
	unsigned int meshaccuracy;

	duration_f sort_time;
	milliseconds calc_time;
//----------
	int test;
	boost::atomic_int t;
};

//--------------------------
//---		-----		----
//million point under 0,4 sec with 4 thread
inline void kdTree::buildWithAutomech()
{
	if (root->depth != stopBuildDepth) buildRecursive(root, true, stopBuildDepth);
	else t_schedular->addTask(root);

	while (!t_schedular->isEmptyTask())
		t_schedular->addSubscribeWithoutAutomechanism(this, &kdTree::buildRecursive);

	t_schedular->joinAll();		//wait our threads
}

inline void kdTree::buildRecursive(node* r, bool stop, const int& stopDepth)
{
	if (r->length == 1 /*|| r->length <= meshaccuracy || r->box.getVolume() < MINVOLUME*/)
	{
		r->point = r->start + (r->length >> 1);
		return;
	}

	int axis = r->depth % DIMENSION;
	int med = r->length >> 1;
	//---------------------
	time_p t1 = hrclock::now();

	sortPartVector(axis, r->start, r->length);

	time_p t2 = hrclock::now();
	sort_time += (boost::chrono::duration_cast<duration_f>(t2 - t1));
	//---------------------

	r->point = r->start + med;

	//---------------------
	Box Lbox = r->box;
	Box Rbox = r->box;

	if (!axis)
	{
		Lbox.setXMax(r->point->first.x());
		Rbox.setXMin(r->point->first.x());
	}
	else if (axis == 1)
	{
		Lbox.setYMax(r->point->first.y());
		Rbox.setYMin(r->point->first.y());
	}
	else
	{
		Lbox.setZMax(r->point->first.z());
		Rbox.setZMin(r->point->first.z());
	}

	r->left = r->point - r->start == 0 ? NULL : new node(r, r->start, r->depth + 1, r->point - r->start, Lbox);
	r->right = r->length - r->left->length - 1 == 0 ? NULL : new node(r, r->point + 1, r->depth + 1, r->length - r->left->length - 1, Rbox);

	r->length = 1;

	if (stop && stopDepth == r->depth + 1)
	{
		if (r->left)  t_schedular->addTask(r->left);
		if (r->right) t_schedular->addTask(r->right);
		return;
	}
	else
	{
		if (r->left)  buildRecursive(r->left, stop, stopDepth);
		if (r->right) buildRecursive(r->right, stop, stopDepth);
	}
}

inline void kdTree::releaseNode(node* r)
{
	node* _l = r->left;
	node* _r = r->right;

	delete r;

	if (_l) releaseNode(_l);
	if (_r) releaseNode(_r);
}

inline void kdTree::sortPartVector(const int& axis, point_uv_tuple* start, const int& length)
{
	if (axis  == 2)
	std::nth_element(start, start + length /2, start + length, CGALTool::ZXYOrder());
	else if (axis == 1)
	std::nth_element(start, start + length / 2, start + length, CGALTool::YZXOrder());
	else
	std::nth_element(start, start + length / 2, start + length, CGALTool::XYZOrder());
}

//---		-----		----
//--------------------------
//---		-----		----
// Neighbours

inline void kdTree::nearest(point_uv_tuple *query, node* currentNode, nearest_node& best) {
	if (!currentNode) return;
	//test++;

	int axis = currentNode->depth % DIMENSION;

	float d = sumOfsquare(currentNode->point->first.x() - query->first.x(), currentNode->point->first.y() - query->first.y(), currentNode->point->first.z() - query->first.z());

	if (d == 0.0f)	//megtal�ltuk a pontunkat, mindk�t gyerek ir�nyba menni kell mert a pont rajta van a v�g�s�kon
	{
		nearest(query, currentNode->left, best);
		nearest(query, currentNode->right, best);
		return;
	}

	//adott v�g�s�kt�l vett t�vols�g
	float dx = axis == 0 ? currentNode->point->first.x() - query->first.x() : axis == 1 ? currentNode->point->first.y() - query->first.y() : currentNode->point->first.z() - query->first.z();

	if (d < best.distance) {
		best.neighb = currentNode;
		best.distance = d;
	}

	node* _near = dx <= 0 ? currentNode->right : currentNode->left;
	node* _far = dx <= 0 ? currentNode->left : currentNode->right;
	nearest(query, _near, best);
	if (dx * dx >= best.distance) return;	//pitagoras
	nearest(query, _far, best);
}

inline void kdTree::nearest_p(Point *query, kdnode_ptr currentNode, nearest_node& best)
{
	if (best.distance == 0) return;
	if (!currentNode) return;

	int axis = currentNode->depth % DIMENSION;

	float d = sumOfsquare(currentNode->point->first.x() - query->x(), currentNode->point->first.y() - query->y(), currentNode->point->first.z() - query->z());

	/*if (d == 0.0f)	//megtal�ltuk a pontunkat, mindk�t gyerek ir�nyba menni kell mert a pont rajta van a v�g�s�kon
	{
		nearest_p(query, currentNode->left, best);
		nearest_p(query, currentNode->right, best);
		return;
	}*/

	//adott v�g�s�kt�l vett t�vols�g
	float dx = axis == 0 ? currentNode->point->first.x() - query->x() : axis == 1 ? currentNode->point->first.y() - query->y() : currentNode->point->first.z() - query->z();

	if (d < best.distance) {
		best.neighb = currentNode;
		best.distance = d;
	}

	node* _near = dx <= 0 ? currentNode->right : currentNode->left;
	node* _far = dx <= 0 ? currentNode->left : currentNode->right;
	nearest_p(query, _near, best);
	if (dx * dx >= best.distance) return;	//pitagoras
	nearest_p(query, _far, best);
}

inline void kdTree::knearest(point_uv_tuple *query, node* currentNode, kNearest_queue& best) {
	if (!currentNode) return;

	int axis = currentNode->depth % DIMENSION;

	float d = sumOfsquare(currentNode->point->first.x() - query->first.x(), currentNode->point->first.y() - query->first.y(), currentNode->point->first.z() - query->first.z());

	if (d == 0.0f)	//megtal�ltuk a pontunkat, mindk�t gyerek ir�nyba menni kell mert a pont rajta van a v�g�s�kon
	{
		knearest(query, currentNode->left, best);
		knearest(query, currentNode->right, best);
		return;
	}

	//adott v�g�s�kt�l vett t�vols�g
	float dx = axis == 0 ? currentNode->point->first.x() - query->first.x() : axis == 1 ? currentNode->point->first.y() - query->first.y() : currentNode->point->first.z() - query->first.z();

	if (best.size() < kn || d <= best.top().first) {
		best.push(DistanceTuple(d, currentNode));
		/*kNearest_queue tmp = best;
		while (!tmp.empty())
		{
		qDebug() << tmp.top() << " ";
		tmp.pop();
		}
		qDebug() << "\n";*/
		if (best.size() > kn) {
			best.pop();
			/*tmp = best;
			while (!tmp.empty())
			{
			qDebug() << tmp.top() << " ";
			tmp.pop();
			}
			qDebug() << "\n";*/
		}
	}

	node* _near = dx <= 0 ? currentNode->right : currentNode->left;
	node* _far = dx <= 0 ? currentNode->left : currentNode->right;
	knearest(query, _near, best);
	if (dx * dx >= best.top().first) return;	//pitagoras
	knearest(query, _far, best);

}

inline void kdTree::knearest_p(Point *query, node* currentNode, kNearest_queue& best) {
	if (!currentNode) return;

	int axis = currentNode->depth % DIMENSION;

	float d = sumOfsquare(currentNode->point->first.x() - query->x(), currentNode->point->first.y() - query->y(), currentNode->point->first.z() - query->z());

	/*if (d == 0.0f)	//megtal�ltuk a pontunkat, mindk�t gyerek ir�nyba menni kell mert a pont rajta van a v�g�s�kon
	{
		knearest(query, currentNode->left, best);
		knearest(query, currentNode->right, best);
		return;
	}*/

	//adott v�g�s�kt�l vett t�vols�g
	float dx = axis == 0 ? currentNode->point->first.x() - query->x() : axis == 1 ? currentNode->point->first.y() - query->y() : currentNode->point->first.z() - query->z();

	if (best.size() < kn || d <= best.top().first) {
		best.push(DistanceTuple(d, currentNode));
		/*kNearest_queue tmp = best;
		while (!tmp.empty())
		{
		qDebug() << tmp.top() << " ";
		tmp.pop();
		}
		qDebug() << "\n";*/
		if (best.size() > kn) {
			best.pop();
			/*tmp = best;
			while (!tmp.empty())
			{
			qDebug() << tmp.top() << " ";
			tmp.pop();
			}
			qDebug() << "\n";*/
		}
	}

	node* _near = dx <= 0 ? currentNode->right : currentNode->left;
	node* _far = dx <= 0 ? currentNode->left : currentNode->right;
	knearest_p(query, _near, best);
	if (dx * dx >= best.top().first) return;	//pitagoras
	knearest_p(query, _far, best);
}

//---		-----		----
//--------------------------

inline GLPaintFormat kdTree::drawNode(/*const unsigned int& bt*/)
{
	GLPaintFormat gpf;
	//node* draw = bt == 0 ? _draw : bt == 1 ? _draw->nn.neighb : NULL;
	if (!_draw) return gpf;

	Box act_box = _draw->box;

	/*gpf.points.push_back(Data(act_box.getXMin(), act_box.getYMin(), act_box.getZMin()));
	gpf.points.push_back(Data(act_box.getXMax(), act_box.getYMin(), act_box.getZMin()));
	gpf.points.push_back(Data(act_box.getXMin(), act_box.getYMax(), act_box.getZMin()));
	gpf.points.push_back(Data(act_box.getXMax(), act_box.getYMax(), act_box.getZMin()));

	gpf.points.push_back(Data(act_box.getXMin(), act_box.getYMin(), act_box.getZMax()));
	gpf.points.push_back(Data(act_box.getXMax(), act_box.getYMin(), act_box.getZMax()));
	gpf.points.push_back(Data(act_box.getXMin(), act_box.getYMax(), act_box.getZMax()));
	gpf.points.push_back(Data(act_box.getXMax(), act_box.getYMax(), act_box.getZMax()));

	
	gpf.ix.push_back(0);	gpf.ix.push_back(1);	gpf.ix.push_back(2);
	gpf.ix.push_back(2);	gpf.ix.push_back(1);	gpf.ix.push_back(3);
							
	gpf.ix.push_back(4);	gpf.ix.push_back(0);	gpf.ix.push_back(6);
	gpf.ix.push_back(6);	gpf.ix.push_back(0);	gpf.ix.push_back(2);

	gpf.ix.push_back(4);	gpf.ix.push_back(5);	gpf.ix.push_back(0);
	gpf.ix.push_back(0);	gpf.ix.push_back(5);	gpf.ix.push_back(1);
	
	gpf.ix.push_back(1);	gpf.ix.push_back(5);	gpf.ix.push_back(3);
	gpf.ix.push_back(3);	gpf.ix.push_back(5);	gpf.ix.push_back(7);

	gpf.ix.push_back(5);	gpf.ix.push_back(4);	gpf.ix.push_back(7);
	gpf.ix.push_back(7);	gpf.ix.push_back(4);	gpf.ix.push_back(6);

	gpf.ix.push_back(2);	gpf.ix.push_back(3);	gpf.ix.push_back(6);
	gpf.ix.push_back(6);	gpf.ix.push_back(3);	gpf.ix.push_back(7);
	
	//gpf.ix.push_back(1);	gpf.ix.push_back(5);	gpf.ix.push_back(7);
	
	gpf.col.push_back(QVector3D(1, 0, 0));
	*/
	return gpf;
}

//---		-----		----
//--------------------------

inline void kdTree::getActivePoints(std::vector<point_uv_tuple>& active_points, node* r)
{
	if (!r) r = root;
	if (!r) return;

	active_points.emplace_back(*(r->point));

	if (r->left) getActivePoints(active_points, r->left);
	if (r->right) getActivePoints(active_points, r->right);
}

inline void kdTree::changeThreadCapacity(const unsigned int& tc)
{
	t_schedular->setCloudThreadCapacity(tc);
	stopBuildDepth = (unsigned int)log2(tc);
}

inline void kdTree::getTimes(float& join, float& sort, float& all) const
{
	//	std::cout << "Sorting time: " << time_sortMS << "\nJoin time: " << t_schedular->getJoinTime() << "\n";
	join = t_schedular->getJoinTime();
	sort = sort_time.count();
	all = calc_time.count();
}

inline void kdTree::inOrderPrint(node* r, const int& level)
{
	if (r == NULL) return;
	if (r->point == NULL)
	{
		std::cout << "Empty Node\n";
		return;
	}

	inOrderPrint(r->left, level + 1);
	std::cout << level << ": " << *(r->point);
	inOrderPrint(r->right, level + 1);
}
