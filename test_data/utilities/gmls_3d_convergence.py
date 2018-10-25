from GMLS import gmls

# import other relevant models
import numpy as np
import math
import random
import scipy.spatial.kdtree as kdtree
#import matplotlib.pyplot as plt

# just for formatting
class colors:
    HEADER = '\033[95m'
    ENDC = '\033[0m'

# function used to generate sample data
def sinxsinysinz(coord):
    return math.sin(coord[0])*math.sin(coord[1])*math.sin(coord[2])


def test1(ND):
    random.seed(1234) # for consistent results

    polyOrder = 2
    manifoldPolyOrder = 2 # Not used
    dimensions = 3

    # initialize 3rd order reconstruction using 2nd order basis in 3D (GMLS)
    gmls_obj=gmls.GMLS_Python(polyOrder, "QR", manifoldPolyOrder, dimensions)
    gmls_obj.setWeightingOrder(10)

    NT = 100 # Targets
    nx, ny, nz = (ND, ND, ND)

    xmax = 1
    ymax = 1 
    zmax = 1
    xmin = -xmax
    ymin = -ymax
    zmin = -zmax

    dx = np.linspace(xmin, xmax, nx)
    dy = np.linspace(ymin, ymax, ny)
    dz = np.linspace(zmin, zmax, nz)

    N=ND*ND*ND


    # target sites
    target_sites = []
    for i in range(NT):
        target_sites.append([random.uniform(xmin, xmax), random.uniform(ymin, ymax), random.uniform(zmin, zmax)])
    target_sites = np.array(target_sites, dtype='d')
    #print target_sites

    
    # source sites
    t_sites = list()
    for i in range(ND):
        for j in range(ND):
            for k in range(ND):
                t_sites.append([dx[i],dy[j],dz[k]])
    #print t_sites
    source_sites = np.array(t_sites, dtype=np.dtype('d'))


    # neighbor search
    my_kdtree = kdtree.KDTree(source_sites, leafsize=10)
    neighbor_number_multiplier = (1 + polyOrder)
    neighbor_lists = []
    epsilons = []
    for target_num in range(NT):
        query_result = my_kdtree.query(target_sites[target_num], k=int(neighbor_number_multiplier*gmls_obj.getNP(polyOrder, dimensions)), eps=0)
        neighbor_lists.append([query_result[1].size,] + list(query_result[1]))
        epsilons.append(query_result[0][-1])
        #epsilons.append(query_result[0][-1]*epsilon_multiplier)
    neighbor_lists = np.array(neighbor_lists, dtype=np.dtype(int))
    epsilons = np.array(epsilons, dtype=np.dtype('d'))
    #print neighbor_lists
    #print epsilons

    # set data in gmls object
    gmls_obj.setWindowSizes(epsilons)
    gmls_obj.setSourceSites(source_sites)
    gmls_obj.setTargetSites(target_sites)
    gmls_obj.setNeighbors(neighbor_lists)
    
    # generate stencil
    gmls_obj.generatePointEvaluationStencil()
   

    # create sample data at source sites
    data_vector = [] 
    for i in range(N):
        data_vector.append(sinxsinysinz(source_sites[i]))
    data_vector = np.array(data_vector, dtype=np.dtype('d'))
    #print data_vector
    
    # apply stencil to sample data for all targets
    computed_answer = gmls_obj.applyStencilTo0Tensor(neighbor_lists, data_vector)
    l2_error = 0
    for i in range(NT):
        l2_error += np.power(abs(computed_answer[i] - sinxsinysinz(target_sites[i])),2)
    l2_error = math.sqrt(l2_error)  
    
    # APPLY STENCIL TO SAMPLE DATA FOR ONE TARGET
    #np_data_sources = np.array([1.0]*N, dtype=np.dtype('d'))
    #computed_answer = gmls_obj.applyStencilTo0Tensor(0, neighbor_lists, data_vector)
    #print "Computed: "+str(computed_answer), "Actual:"+str(sinxsinysinz(target_sites[0]))
    #l2_error = abs(computed_answer - sinxsinysinz(target_sites[0]))
    #print "Error: "+str(l2_error) 
    #print gmls_obj
    return l2_error

# # of points in each dimension to run the test over
ND = [5, 10, 20, 40]
print "\n" + colors.HEADER + "(l2) Errors:" + colors.ENDC
errors = list()
for nd in ND:
    errors.append(test1(nd))
    print str(errors[-1])

print "\n" + colors.HEADER + "(l2) Rates:" + colors.ENDC
for i in range(len(ND)-1):
    print str(math.log(errors[i+1]/errors[i])/math.log(0.5))
print "\n"